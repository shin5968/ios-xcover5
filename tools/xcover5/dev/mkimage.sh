#!/bin/bash
# Build one Odin AP tar (boot.img + the drop zone's cache.img) for the phone.
#
#   bash tools/xcover5/dev/mkimage.sh NAME [extra boot-args...]
#
# Uses xnu/out/kernel.development.a5xelte (from the xnu CI artifact "xnu-kernel") and
# shim/xcover5/out/xc5-drop-mesa.tar (tools/xcover5/mkdrop.py over the mesa-ios artifact).
# Writes shim/xcover5/out/xc5-ios-NAME-console.tar. Run from WSL.
#
# Current baseline (boot 77, lock screen with iOS's stock wallpaper, unlocks to the home screen):
#   bash tools/xcover5/dev/mkimage.sh wall2 xc5gpu=4 xc5gl=2 xc5wall=1
set -e
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO"
NAME=${1:?name}; shift
export XC5_DRAM=0x8a2800000:0x40000000      # 1 GiB above 4 GiB (the user chose 1 GiB)
A="debug=0xc serial=3 keepsyms=1 -enable_kprintf_spam -noprogress rd=emmcroot0 emmcroot=USERDATA emmcwrite=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 a5touchprobe=0 a5fb_keep_on=1 cpus=8 sweetfb_hz=24"
K=xnu/out/kernel.development.a5xelte; O=shim/xcover5/out
rm -f "$O/xc5-ios-$NAME-console.tar"
bash tools/xcover5/make-boot.sh $K ../boot_phone.img "$O/xc5-ios-$NAME-console.img" $A "$@" xc5conrows=40 2>&1 | grep -iE "error|traceback" || true
python3 - "$O/xc5-ios-$NAME-console.tar" "$O/xc5-drop-mesa.tar" <<'PY'
import sys, tarfile, io, struct
boot, drop = sys.argv[1], sys.argv[2]
with tarfile.open(drop) as d:
    m = d.getmember('cache.img'); data = d.extractfile(m).read()
with tarfile.open(boot, 'a', format=tarfile.USTAR_FORMAT) as t:
    info = tarfile.TarInfo('cache.img'); info.size = len(data); info.mtime = m.mtime; info.mode = 0o644
    t.addfile(info, io.BytesIO(data))
i = data.find(b'XC5DROP1')
print(boot, 'cache.img package', struct.unpack_from('<8sIIQQ', data, i)[4] if i >= 0 else None)
PY
ls -la "$O/xc5-ios-$NAME-console.tar"
strings -n 20 "$O/xc5-ios-$NAME-console.img" | grep -m1 -o 'cpus=8.*'
