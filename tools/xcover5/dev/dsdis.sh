#!/bin/bash
# dsdis.sh UNSLID_ADDR HEXLEN : disassemble shared-cache bytes (aarch64-linux-gnu-objdump). Needs mntsys.sh first.
HERE=$(cd "$(dirname "$0")" && pwd)
python3 -c "
import sys; sys.path.insert(0, '$HERE')
import dsc
b = dsc.read(int('$1', 16), int('$2', 16))
sys.stdout.buffer.write(b or b'')
" > /tmp/dsdis.bin
aarch64-linux-gnu-objdump -D -b binary -m aarch64 --adjust-vma=$1 /tmp/dsdis.bin | tail -n +8
