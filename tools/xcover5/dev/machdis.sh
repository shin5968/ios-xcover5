#!/bin/bash
# machdis.sh BINARY VMADDR BEFORE AFTER : disassemble a non-cache Mach-O's __TEXT (vmaddr 0x100000000, fileoff 0)
# from VMADDR-BEFORE to VMADDR+AFTER (hex), e.g. machdis.sh checkpointd 1000036d8 80 20
B=$1; A=$((0x$2)); S=$((A - 0x$3)); E=$((A + 0x$4))
aarch64-linux-gnu-objdump -D -b binary -m aarch64 --adjust-vma=0x100000000 \
  --start-address=$S --stop-address=$E "$B" | tail -n +8
