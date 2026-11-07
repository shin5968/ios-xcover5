#!/usr/bin/env python3
"""Which imported symbol a __stubs / __auth_stubs entry of a Mach-O calls.

  machstub.py BINARY STUB_VMADDR [...]
"""
import struct, sys

path = sys.argv[1]
data = open(path, 'rb').read()
magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', data, 0)
off = 32
sects, symoff, nsyms, stroff, indoff, nind = [], 0, 0, 0, 0, 0
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, off)
    if cmd == 0x19:
        nsect = struct.unpack_from('<I', data, off + 64)[0]
        s = off + 72
        for i in range(nsect):
            sect = data[s:s + 16].rstrip(b'\0').decode()
            addr, sz = struct.unpack_from('<QQ', data, s + 32)
            fl, r1, r2 = struct.unpack_from('<III', data, s + 64)
            sects.append((sect, addr, sz, fl, r1, r2))
            s += 80
    elif cmd == 0x2:  # LC_SYMTAB
        symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', data, off + 8)
    elif cmd == 0xb:  # LC_DYSYMTAB
        indoff, nind = struct.unpack_from('<II', data, off + 56)
    off += size


def symname(i):
    strx = struct.unpack_from('<I', data, symoff + i * 16)[0]
    e = data.index(b'\0', stroff + strx)
    return data[stroff + strx:e].decode()


for a in sys.argv[2:]:
    a = int(a, 16)
    for sect, addr, sz, fl, r1, r2 in sects:
        if (fl & 0xff) == 8 and addr <= a < addr + sz:   # S_SYMBOL_STUBS, r2 = stub size
            k = r1 + (a - addr) // r2
            idx = struct.unpack_from('<I', data, indoff + 4 * k)[0]
            print(hex(a), sect, 'entry', (a - addr) // r2, '->', symname(idx))
            break
    else:
        print(hex(a), 'not in a stubs section')
