#!/bin/bash
# Cross-build Mesa's Panfrost (Gallium + Midgard compiler) for the Galaxy
# S7's iOS 16 userland (plain arm64), inside OSMesa, over the HeroGPU
# libdrm stand-in. Runs on a macOS runner with Xcode (iphoneos SDK).
#
#   build-mesa.sh WORK OUT
#
# OUT gets lib/{libOSMesa.8,libglapi.0,libdrm.2}.dylib (install names
# @rpath/...) and osmesa-test (rpath /private/var/herolte/lib), all ad-hoc
# signed. Nothing of Apple's is in OUT.
set -euo pipefail

WORK=$(mkdir -p "$1" && cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
MESA_TAG=${MESA_TAG:-mesa-25.0.7}
LIBDRM_TAG=${LIBDRM_TAG:-libdrm-2.4.124}
MIN_IOS=16.0
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC_IOS="xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=$MIN_IOS -isysroot $SDK"

cd "$WORK"
[ -d mesa ] || git clone -q --depth 1 --branch "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git mesa
[ -d libdrm ] || git clone -q --depth 1 --branch "$LIBDRM_TAG" https://gitlab.freedesktop.org/mesa/drm.git libdrm
python3 "$HERE/patch-mesa.py" mesa

# XCover5 Metal M1 (docs/xcover5-metal-plan.md): the AIR frontend (Apple's shader bitcode -> NIR) from
# stormtroopxyz/y2sios (MIT, gpu/metal/air, with Luminous418/hakios-a32's air-integer-ubo.patch applied), and
# libredwoodair (its rwa_* contract, air_metal.h) over panfrost's Bifrost compiler, as y2sios's air-panfrost.patch
# declares it -- but linked against our libdrm stand-in (herodrm) instead of compiling y2sios's redwood_drm.c in.
METAL="$HERE/../metal"
if [ -d "$METAL/air" ] && [ ! -f mesa/.xc5-air ]; then
  rm -rf mesa/src/compiler/air
  cp -r "$METAL/air" mesa/src/compiler/air
  python3 - mesa <<'PY'
import sys
from pathlib import Path
m = Path(sys.argv[1])
c = m / 'src/compiler/meson.build'
t = c.read_text()
if "subdir('air')" not in t:
    t = t.replace("subdir('spirv')\n", "subdir('spirv')\n\nsubdir('air')\n", 1)
    assert "subdir('air')" in t, 'src/compiler/meson.build: no spirv subdir'
    c.write_text(t)
p = m / 'src/gallium/drivers/panfrost/meson.build'
t = p.read_text()
if 'redwoodair' not in t:
    t += '''
# XCover5: libredwoodair, the Metal driver's compiler and dispatcher (y2sios air-panfrost.patch, without
# redwood_drm.c: dep_libdrm is our HeroGPU/XC5GPU stand-in).
libredwoodair = shared_library(
  'redwoodair',
  files('../../../compiler/air/air_metal.c'),
  include_directories : [inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium,
                         inc_gallium_aux, inc_gallium_winsys_sw, inc_gallium_winsys,
                         include_directories('../../drivers')],
  dependencies : [dep_m, idep_air, idep_mesautil, idep_xmlconfig, driver_softpipe,
                  driver_panfrost, dep_libdrm],
  link_with : [libgallium, libgalliumvl_stub, libws_null],
  build_by_default : false,
  install : false,
)
'''
    p.write_text(t)
PY
  touch mesa/.xc5-air
fi

# The libdrm stand-in, and a pkg-config file pointing Mesa at it.
mkdir -p drm-standin pc
$CC_IOS -O2 -Wall -fvisibility=hidden -dynamiclib \
  -install_name @rpath/libdrm.2.dylib \
  -I libdrm -I libdrm/include/drm -I mesa/include \
  -o drm-standin/libdrm.2.dylib "$HERE/herodrm.c" \
  -framework IOKit -framework CoreFoundation
ln -sf libdrm.2.dylib drm-standin/libdrm.dylib
cat > pc/libdrm.pc <<EOF
Name: libdrm
Description: HeroGPU libdrm stand-in
Version: ${LIBDRM_TAG#libdrm-}
Cflags: -I$WORK/libdrm -I$WORK/libdrm/include/drm
Libs: -L$WORK/drm-standin -ldrm
EOF

cat > ios-cross.txt <<EOF
[binaries]
c = ['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64']
cpp = ['xcrun', '--sdk', 'iphoneos', 'clang++', '-arch', 'arm64']
objc = ['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64']
ar = ['xcrun', '--sdk', 'iphoneos', 'ar']
strip = ['xcrun', '--sdk', 'iphoneos', 'strip']
ranlib = ['xcrun', '--sdk', 'iphoneos', 'ranlib']
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-miphoneos-version-min=$MIN_IOS', '-isysroot', '$SDK']
cpp_args = ['-miphoneos-version-min=$MIN_IOS', '-isysroot', '$SDK']
objc_args = ['-miphoneos-version-min=$MIN_IOS', '-isysroot', '$SDK']
c_link_args = ['-miphoneos-version-min=$MIN_IOS', '-isysroot', '$SDK']
cpp_link_args = ['-miphoneos-version-min=$MIN_IOS', '-isysroot', '$SDK']
objc_link_args = ['-miphoneos-version-min=$MIN_IOS', '-isysroot', '$SDK']

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$WORK/pc'

[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

cd mesa
[ -d build ] || meson setup build --cross-file ../ios-cross.txt \
  --buildtype=release -Db_ndebug=true -Dprefix=/ \
  -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= \
  -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= \
  -Dosmesa=true -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dshared-glapi=enabled \
  -Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
  -Dxmlconfig=disabled -Dbuild-tests=false -Dglvnd=disabled -Dlmsensors=disabled \
  -Dgallium-opencl=disabled -Dgallium-rusticl=false -Dgallium-xa=disabled \
  -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dtools= -Dperfetto=false
ninja -C build -k 0
# Metal M1: not built by default (build_by_default false), asked for by name. A failure here does not cost the
# GLES libraries above: the log says why and the artifact goes out without it.
AIR_OK=0
if [ -f .xc5-air ]; then
  if ninja -C build src/gallium/drivers/panfrost/libredwoodair.dylib; then AIR_OK=1; else echo "::warning::libredwoodair did not build"; fi
fi
cd ..

mkdir -p "$OUT/lib"
osmesa=$(ls mesa/build/src/gallium/targets/osmesa/libOSMesa.8*.dylib | head -1)
glapi=$(ls mesa/build/src/mapi/shared-glapi/libglapi.0*.dylib 2>/dev/null | head -1 || true)
cp "$osmesa" "$OUT/lib/libOSMesa.8.dylib"
[ -n "$glapi" ] && cp "$glapi" "$OUT/lib/libglapi.0.dylib"
cp drm-standin/libdrm.2.dylib "$OUT/lib/"
if [ "$AIR_OK" = 1 ]; then
  cp mesa/build/src/gallium/drivers/panfrost/libredwoodair.dylib "$OUT/lib/libredwoodair.dylib"
  echo "libredwoodair: $(nm -gU "$OUT/lib/libredwoodair.dylib" | grep -c ' _rwa_') rwa_* functions"
fi

# @rpath install names, so the dylibs load from wherever they are installed.
for lib in "$OUT"/lib/*.dylib; do
  name=$(basename "$lib")
  install_name_tool -id "@rpath/$name" "$lib"
  for dep in $(otool -L "$lib" | awk 'NR > 1 { print $1 }'); do
    case "$dep" in
      /usr/lib/*|/System/*|@rpath/*) ;;
      *) install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$lib" ;;
    esac
  done
done

# XCover5: files live in /private/var/xc5/lib (the CACHE drop zone,
# tools/xcover5/mkdrop.py), and the test runs inside backboardd as
# libxc5test.dylib (gpu/mesa/xc5test.c) instead of a launchd job.
$CC_IOS -O2 -Wall -I mesa/include -o "$OUT/osmesa-test" "$HERE/osmesa-test.c" \
  -L "$OUT/lib" -lOSMesa.8 -ldrm.2 -Wl,-rpath,/private/var/xc5/lib
$CC_IOS -O2 -Wall -I mesa/include -dynamiclib -install_name @rpath/libxc5test.dylib \
  -o "$OUT/lib/libxc5test.dylib" "$HERE/xc5test.c" \
  -L "$OUT/lib" -lOSMesa.8 -ldrm.2 -Wl,-rpath,/private/var/xc5/lib

for f in "$OUT"/lib/*.dylib "$OUT/osmesa-test"; do
  codesign -s - -f "$f"
  otool -L "$f"
done
ls -la "$OUT" "$OUT/lib"
