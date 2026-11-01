#!/bin/bash
# Build the Galaxy XCover5 XNU loader (Samsung S-Boot "kernel" Image).
# Optionally pack a BOOT image: build.sh STOCK_BOOT.img RAMDISK OUT.img
# (RAMDISK = tools/mkpayload.py payload, or any file for a probe-only boot;
# STOCK_BOOT supplies the v2 header, the DTB and the AVB footer layout).
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -march=armv8-a -Wall -Wextra -Werror"
mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c loader.c -o out/loader.o
$CC $CFLAGS -c ../fdt.c -o out/fdt.o
objs="out/loader.o out/fdt.o"
# Runs wherever S-Boot puts it with no relocation pass.
abs=$(aarch64-linux-gnu-readelf -r $objs | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations" >&2; exit 1; }
$CC $CFLAGS -static -T loader.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o $objs -o out/loader.elf
$OBJCOPY -O binary out/loader.elf out/Image
size=$(aarch64-linux-gnu-nm out/loader.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
echo "out/Image: $(stat -c %s out/Image) bytes"
if [ $# = 3 ]; then
	python3 ../../tools/xcover5/mkboot.py "$1" out/Image "$2" "$3"
	sha256sum "$3"
fi
