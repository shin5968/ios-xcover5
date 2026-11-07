#!/bin/bash
# The XCover5's vendor partition out of the stock firmware (Samsung's AP tar), for the firmware files the
# drivers need (Wi-Fi: Samsung SCSC WLBT). Everything goes to ~/xc5/stock in WSL and NEVER into git.
#   bash tools/xcover5/dev/stockfw.sh /mnt/e/.../AP_G525N...tar.md5
set -e
AP=${1:?AP tar}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$HOME/xc5/stock; mkdir -p "$W"; cd "$W"
[ -x venv/bin/python ] || { python3 -m venv venv && venv/bin/pip -q install lz4; }
[ -f super.img.lz4 ] || [ -f super.raw ] || tar -xf "$AP" super.img.lz4
if [ ! -f super.raw ]; then
  [ -f super.sparse ] || venv/bin/python -c "
import lz4.frame, shutil
with lz4.frame.open('super.img.lz4') as i, open('super.sparse', 'wb') as o: shutil.copyfileobj(i, o, 64 << 20)"
  simg2img super.sparse super.raw && rm -f super.sparse super.img.lz4
fi
python3 "$HERE/lpunpack.py" super.raw -
[ -f vendor.img ] || python3 "$HERE/lpunpack.py" super.raw vendor vendor.img
file vendor.img 2>/dev/null || true
head -c 2048 vendor.img | od -A x -t x1 | sed -n '64,70p'
mkdir -p vendor
if debugfs -R 'ls -l /' vendor.img >/dev/null 2>&1; then
  for d in /etc/wifi /firmware /etc/firmware; do debugfs -R "rdump $d vendor" vendor.img 2>/dev/null || true; done
  debugfs -R 'ls -l /etc/wifi' vendor.img 2>/dev/null | head -40
else
  echo "vendor.img is not ext4 (erofs?): needs another extractor"
fi
find vendor -maxdepth 3 -type f | head -60
