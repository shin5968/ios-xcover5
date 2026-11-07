#!/bin/bash
# Metal M2: y2sios's Metal driver (runtime/redwoodmetal.m + redwooddrawable.m, MIT) as the
# loadable bundle Metal's discovery wants: OUT/metal/RedwoodMetal.bundle/{RedwoodMetal,Info.plist}.
#
#   build-metal.sh OUT       (the mesa-ios artifact directory; lib/libredwoodair.dylib from build-mesa.sh)
#
# Changes from y2sios, all for XCover5's kernel (docs/xcover5-metal-plan.md):
#   - runtime/xc5inc/IOKit/IOKitLib.h is found before the SDK's and turns the driver's
#     RedwoodMali struct requests into XC5GPU's scalar selectors (CreateBO, MapBO, FreeBO,
#     GetParam, SurfaceBO);
#   - m2m.h sends records to XC5GPU's Log selector when there is no /dev/m2m;
#   - libredwoodair is dlopen'ed from /private/var/xc5/lib (the CACHE drop zone).
# The driver subclasses Metal's private _MTLDevice; the iOS SDK's Metal.tbd may not list it,
# so those two symbols are left for dyld to find in Metal at load time (-U).
set -euo pipefail
OUT=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
RT="$HERE/runtime"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -isysroot $SDK"
OBJ=$(mktemp -d)
B="$OUT/metal/RedwoodMetal.bundle"

CF="-O1 -g -w -fno-objc-arc -fobjc-exceptions -I$RT/xc5inc -I$RT"
$CC $CF -c "$RT/redwoodmetal.m" -o "$OBJ/redwoodmetal.o"
$CC $CF -c "$RT/redwooddrawable.m" -o "$OBJ/redwooddrawable.o"
mkdir -p "$B"
$CC -bundle -o "$B/RedwoodMetal" "$OBJ/redwoodmetal.o" "$OBJ/redwooddrawable.o" \
  -framework Foundation -framework IOKit -framework Metal -lobjc \
  -Wl,-U,'_OBJC_CLASS_$__MTLDevice' -Wl,-U,'_OBJC_METACLASS_$__MTLDevice' \
  -Wl,-rpath,/private/var/xc5/lib
cp "$RT/metal_plugin.plist" "$B/Info.plist"
codesign -s - -f "$B/RedwoodMetal"
echo "RedwoodMetal: $(nm -gU "$B/RedwoodMetal" | grep -c 'RedwoodMetal') RedwoodMetal* symbols"
nm -u "$B/RedwoodMetal" | grep -i 'MTLDevice' || true
otool -L "$B/RedwoodMetal"

# libxc5mtl.dylib: chronod asks Metal for the device (xc5_mtl.c; boot-args xc5metal=1 xc5mtl=1)
$CC -O2 -g -Wall -c "$HERE/xc5_mtl.c" -o "$OBJ/xc5_mtl.o"
$CC -dynamiclib -o "$OUT/lib/libxc5mtl.dylib" "$OBJ/xc5_mtl.o" -install_name @rpath/libxc5mtl.dylib \
  -framework CoreFoundation -framework IOKit -lobjc -Wl,-undefined,error
codesign -s - -f "$OUT/lib/libxc5mtl.dylib"
