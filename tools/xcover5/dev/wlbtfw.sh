#!/bin/bash
# The two binary inputs of the Wi-Fi core (bsd/dev/xc5_wlbt.c), made on this machine and NEVER committed:
#   shim/xcover5/out/wifi/mx140.bin  the WLBT firmware, from the stock vendor image (tools/xcover5/dev/stockfw.sh)
#   shim/xcover5/out/wifi/ka.bin     the PMU boot patch ("KA patch", an 8051 program), as little-endian words, taken
#                                    from Samsung's GPL driver source (Galaxy A21s kernel, platform_mif_3830.c ka_patch[])
# Then: python tools/xcover5/mkdrop.py ... wifi/mx140.bin=shim/xcover5/out/wifi/mx140.bin wifi/ka.bin=shim/xcover5/out/wifi/ka.bin
set -e
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=$REPO/shim/xcover5/out/wifi; mkdir -p "$OUT"
cp ~/xc5/stock/vendor/wifi/mx140.bin "$OUT/mx140.bin"
SRC=https://raw.githubusercontent.com/jameskdev/android_kernel_samsung_a21s-common/HEAD/drivers/misc/samsung/scsc/platform_mif_3830.c
curl -sSfL "$SRC" -o "$OUT/.platform_mif_3830.c"
python3 - "$OUT/.platform_mif_3830.c" "$OUT/ka.bin" <<'PY'
import re, struct, sys
src = open(sys.argv[1]).read()
body = re.search(r'uint32_t\s+ka_patch\[\]\s*=\s*\{(.*?)\};', src, re.S).group(1)
words = [int(w, 16) for w in re.findall(r'0x[0-9a-fA-F]+', re.sub(r'//[^\n]*', '', body))]
open(sys.argv[2], 'wb').write(b''.join(struct.pack('<I', w) for w in words))
print(f'ka.bin: {len(words)} words')
PY
rm -f "$OUT/.platform_mif_3830.c"
ls -l "$OUT"
