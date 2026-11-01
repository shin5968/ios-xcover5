#!/bin/bash
# Build the XCover5 iOS root on WSL Ubuntu, in one run as root:
#   sudo bash tools/xcover5/build-root.sh IPSW
# IPSW: iPhone X (iPhone10,3) 16.7.16 20H392 (.ipsw file).
#
# 1. lzfse (apt) -> research/ipsw-x/d22ap.adt (MobileGestalt's /product)
# 2. hfsplus.ko for the running WSL kernel (CONFIG_HFSPLUS_FS is off in
#    WSL): built from Microsoft's source of the exact release, then insmod
# 3. tools/mkiosroot.sh + tools/dreamlte/finish-iosroot.sh -> 16 GiB HFS+
# 4. img2simg -> Android sparse userdata.img -> Odin tar (AP slot)
# Work files go to ~SUDO_USER/xc5; the Odin tar to shim/xcover5/out/.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
repo=$(cd "$(dirname "$0")/../.." && pwd)
user=${SUDO_USER:-root}
home=$(getent passwd "$user" | cut -d: -f6)
work=$home/xc5
mkdir -p "$work/ipsw"
src=$(realpath "${1:?usage: build-root.sh IPSW}")

echo "== packages"
apt-get update -qq
apt-get install -y -qq lzfse git make gcc bc flex bison libelf-dev libssl-dev dwarves rsync hfsprogs android-sdk-libsparse-utils >/dev/null

echo "== IPSW images"
[ -f "$src" ] || { echo "$src: not a file" >&2; exit 1; }
python3 - "$src" "$work/ipsw" <<'PY'
import os, plistlib, shutil, sys, zipfile
ipsw, out = sys.argv[1:]
with zipfile.ZipFile(ipsw) as z:
    m = plistlib.loads(z.read('BuildManifest.plist'))
    bi = next(b for b in m['BuildIdentities']
              if b['Info'].get('DeviceClass') == 'd22ap' and 'Erase' in b['Info']['Variant'])
    for key, dst in {'OS': 'OS.dmg', 'Cryptex1,SystemOS': 'SystemOS.dmg',
                     'Cryptex1,AppOS': 'AppOS.dmg'}.items():
        member = bi['Manifest'][key]['Info']['Path']
        p = os.path.join(out, dst)
        if os.path.exists(p) and os.path.getsize(p) == z.getinfo(member).file_size:
            continue
        with z.open(member) as f, open(p + '.part', 'wb') as g:
            shutil.copyfileobj(f, g, 16 << 20)
        os.rename(p + '.part', p)
        print('extracted', dst)
PY

echo "== d22ap.adt"
mkdir -p "$repo/research/ipsw-x"
python3 "$repo/tools/im4p_extract.py" "$src" Firmware/all_flash/DeviceTree.d22ap.im4p 	"$repo/research/ipsw-x/d22ap.adt"

echo "== hfsplus.ko for $(uname -r)"
if ! grep -qw hfsplus /proc/filesystems; then
	ver=$(uname -r | sed 's/-microsoft-standard-WSL2$//')
	k=$work/wslkernel
	[ -d "$k/.git" ] || git clone -q --depth 1 --branch "linux-msft-wsl-$ver" \
		https://github.com/microsoft/WSL2-Linux-Kernel "$k"
	cd "$k"
	zcat /proc/config.gz > .config
	./scripts/config --module CONFIG_HFSPLUS_FS
	make -s olddefconfig
	make -s -j"$(nproc)" modules_prepare
	make -s -j"$(nproc)" KBUILD_MODPOST_WARN=1 M=fs/hfsplus modules
	insmod fs/hfsplus/hfsplus.ko
	cd "$repo"
fi
grep -w hfsplus /proc/filesystems

# Both scripts end with `fsck.hfsplus -f -n`, which Linux's fsck fails on
# names it calls "Illegal name" (Apple's decomposition of localized names,
# see mkiosroot.sh) even after -y. That exit status says nothing about the
# copy, so it is ignored and the image content is checked here instead.
mnt=$work/check
root_has() {	# root_has PATH...: all exist in the image
	mkdir -p "$mnt"
	mount -t hfsplus -o loop,ro,force "$img" "$mnt" || return 1
	local ok=0
	for f in "$@"; do
		[ -e "$mnt/$f" ] || { echo "missing in image: $f"; ok=1; }
	done
	umount "$mnt"
	return $ok
}
copied="sbin/launchd System/Library/CoreServices/SpringBoard.app/SpringBoard System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"
img=$work/ios16-root.img

echo "== iOS root image (15-40 min)"
if [ -f "$img" ] && root_has $copied; then
	echo "$img already holds the iOS root: copy step skipped"
else
	rm -f "$img"
	bash "$repo/tools/mkiosroot.sh" "$work/ipsw/OS.dmg" "$work/ipsw/SystemOS.dmg" "$work/ipsw/AppOS.dmg" "$img" 16G ||
		echo "(mkiosroot.sh: final fsck -n failed, checking the content instead)"
	root_has $copied
fi

echo "== bring-up tweaks"
bash "$repo/tools/dreamlte/finish-iosroot.sh" "$img" ||
	echo "(finish-iosroot.sh: final fsck -n failed, checking the content instead)"
root_has $copied private/var/keybags/systembag.kb private/var/db/diagnostics/.migrated
fsck.hfsplus -f -y "$img" | grep -v 'Illegal name' || true

echo "== Odin userdata tar"
img2simg "$img" "$work/userdata.img"
out=$repo/shim/xcover5/out
mkdir -p "$out"
tar -C "$work" -cf "$out/xc5-ios-userdata.tar" userdata.img
chown -R "$user": "$work" "$out"
ls -la "$out/xc5-ios-userdata.tar"
echo "done"
