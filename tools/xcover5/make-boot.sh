#!/bin/bash
# Kernel Mach-O -> Galaxy XCover5 BOOT image (+ Odin tar).
#   make-boot.sh KERNEL STOCK_BOOT.img OUT.img [boot-args...]
#   make-boot.sh --probe STOCK_BOOT.img OUT.img
# --probe packs the loader with a dummy ramdisk: it only prints what it
# measured on the phone (EL, MPIDR, DTB, initrd, ...) and stops.
# OUT.tar (Odin, AP slot) is written next to OUT.img.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=$repo/shim/xcover5/out
mkdir -p "$work"
if [[ $1 == --probe ]]; then
	stock=$(realpath "$2"); out=$(realpath -m "$3")
	head -c 4096 /dev/zero | tr '\0' 'P' > "$work/probe-ramdisk"
	bash "$repo/shim/xcover5/build.sh"
	python3 "$repo/tools/xcover5/mkboot.py" "$stock" "$work/Image" "$work/probe-ramdisk" "$out" --tar "${out%.img}.tar"
	exit 0
fi
kernel=$(realpath "$1"); stock=$(realpath "$2"); out=$(realpath -m "$3")
shift 3
# Default: one CPU, root on USERDATA (eMMC), verbose console, no watchdog.
args=${*:-debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=emmcroot0 emmcroot=USERDATA emmcwrite=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 a5touchprobe=0}
python3 "$repo/tools/mkadt_xcover5.py" "$work/xcover5.adt"
python3 "$repo/tools/mkpayload.py" "$kernel" "$work/xcover5.adt" "$work/payload.bin" --cmdline "$args"
bash "$repo/shim/xcover5/build.sh"
python3 "$repo/tools/xcover5/mkboot.py" "$stock" "$work/Image" "$work/payload.bin" "$out" --tar "${out%.img}.tar"
