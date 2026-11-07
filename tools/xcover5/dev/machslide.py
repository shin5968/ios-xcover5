#!/usr/bin/env python3
"""Find the ASLR slide of a (non-cache) Mach-O from return addresses seen on the phone.

  machslide.py BINARY RET [RET...]      (the "+..." addresses xc5poster prints)

Tries every slide (0x4000 steps) that puts all RETs inside __TEXT and prints those where the
instruction before each RET is a BL/BLR/BLRAA... (a call). Then prints, for the best slide, each
RET's file offset; disassemble around it with
  aarch64-linux-gnu-objdump -D -b binary -m aarch64 --start-address=... BINARY_TEXT
"""
import struct, sys

path, rets = sys.argv[1], [int(a.lstrip('+'), 16) for a in sys.argv[2:]]
data = open(path, 'rb').read()
magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', data, 0)
assert magic == 0xfeedfacf, 'thin arm64 Mach-O expected'
off = 32
text = None
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, off)
    if cmd == 0x19:  # LC_SEGMENT_64
        name = data[off + 8:off + 24].rstrip(b'\0').decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', data, off + 24)
        if name == '__TEXT':
            text = (vmaddr, vmsize, fileoff)
    off += size
vm, vs, fo = text


def is_call(w):
    return (w & 0xfc000000) == 0x94000000 or (w & 0xfffffc1f) == 0xd63f0000 or (w & 0xfefff800) == 0xd63f0800


best = []
lo = max(r for r in rets) - (vm + vs)
hi = min(r for r in rets) - vm
s = (lo // 0x4000) * 0x4000
while s <= hi:
    ok = 0
    for r in rets:
        a = r - s - 4 - vm + fo
        if 0 <= a < len(data) - 4 and is_call(struct.unpack_from('<I', data, a)[0]):
            ok += 1
    if ok == len(rets):
        best.append(s)
    s += 0x4000
print('slides where every RET follows a call:', [hex(x) for x in best])
for s in best[:3]:
    print('slide', hex(s), ' '.join('%s->%#x' % (hex(r), r - s) for r in rets))
