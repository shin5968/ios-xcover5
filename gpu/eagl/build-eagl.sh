#!/bin/bash
# CoreAnimation's GLES renderer on the XCover5's Mali-G52: the two EAGL
# bridges from wardevsa/ios-a5xelte (branch herolte, gpu/eagl/build-eagl.sh),
# over the Panfrost libOSMesa from gpu/mesa/build-mesa.sh.
#
#   build-eagl.sh OUT        (OUT as given to build-mesa.sh: lib/libOSMesa.8.dylib, lib/libdrm.2.dylib)
#
# Adds lib/libxc5eagl.dylib (HakiOS's bridge, gpu/hakios-gl: the one the S7
# port ended with) and lib/libxc5eagl-b.dylib (Bergamota's shim, gpu/eagl).
# The kernel inserts the first into backboardd with xc5gl=2, the second
# with xc5gl=3 (bsd/dev/xc5_drop.c). The S7's test programs, audio plug-in
# and SpringBoard/app helpers are left out.
set -euo pipefail
OUT=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
MESA_INC=${MESA_INC:?set MESA_INC to the Mesa source include/ directory}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot $SDK"
RPATH=/private/var/xc5/lib
OBJ=$(mktemp -d)

python3 "$HERE/tools/gen_gl_stubs.py" > "$OBJ/gl_stubs.c"
CF="-O2 -g -fvisibility=hidden -Wall -Wno-deprecated-declarations -Wno-unused-function -I$HERE/src -I$MESA_INC"
$CC $CF -c "$OBJ/gl_stubs.c" -o "$OBJ/gl_stubs.o"
for f in gl_resolve gl_unimpl osm macro_ctx macro_slots hero_log hero_backboardd hero_prof xc5_pbreset; do
  $CC $CF -c "$HERE/src/$f.c" -o "$OBJ/$f.o"
done
$CC $CF -fno-objc-arc -fobjc-exceptions -DEAGL_INJECT -c "$HERE/src/eagl.m" -o "$OBJ/eagl.o"
$CC -dynamiclib -o "$OUT/lib/libxc5eagl-b.dylib" "$OBJ"/{gl_stubs,gl_resolve,gl_unimpl,osm,macro_ctx,macro_slots,hero_log,hero_backboardd,hero_prof,xc5_pbreset,eagl}.o \
  -install_name @rpath/libxc5eagl-b.dylib \
  -framework Foundation -framework CoreFoundation -framework CoreGraphics -framework IOSurface \
  -L "$OUT/lib" -lOSMesa.8 -Wl,-rpath,$RPATH -Wl,-undefined,error

HG="$HERE/../hakios-gl"
nm -gU "$OUT/lib/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$OBJ/exported-gl.txt"
python3 "$HG/mkeagl.py" "$HG/src/eagl_slots.txt" "$MESA_INC/../src/mapi/glapi/registry/gl.xml" \
  "$OBJ/exported-gl.txt" "$HG/src/eagl.c" > "$OBJ/eagl_gen.h"
$CC -O2 -g -w -I"$OBJ" -I"$HG/src" -I"$HG/include" -c "$HG/src/eagl.c" -o "$OBJ/hg_eagl.o"
$CC -O2 -g -Wall -I"$HG/src" -c "$HG/src/gl_hero.c" -o "$OBJ/hg_gl_hero.o"
$CC -O2 -g -Wall -c "$HG/src/ca_hooks.c" -o "$OBJ/hg_ca_hooks.o"
$CC -dynamiclib -o "$OUT/lib/libxc5eagl.dylib" "$OBJ"/{hg_eagl,hg_gl_hero,hg_ca_hooks,hero_log,hero_backboardd,hero_prof,xc5_pbreset}.o \
  -install_name @rpath/libxc5eagl.dylib \
  -framework CoreFoundation -framework IOKit -framework IOSurface -lobjc \
  -L "$OUT/lib" -lOSMesa.8 -Wl,-rpath,$RPATH -Wl,-undefined,error

# libxc5pb.dylib: the PosterBoard first-time-setup hook (src/xc5_pbhook.c), for boot-arg xc5pbhook=1.
$CC -O2 -g -Wall -fno-objc-arc -dynamiclib -o "$OUT/lib/libxc5pb.dylib" "$HERE/src/xc5_pbhook.c" \
  -install_name @rpath/libxc5pb.dylib -framework CoreFoundation -framework IOKit -lobjc -Wl,-undefined,error

for f in "$OUT/lib/libxc5eagl.dylib" "$OUT/lib/libxc5eagl-b.dylib" "$OUT/lib/libxc5pb.dylib"; do
  codesign -s - -f "$f"
  otool -L "$f"
done
