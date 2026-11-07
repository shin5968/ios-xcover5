#!/usr/bin/env python3
"""Extract one partition from an Android dynamic-partition super image (raw, not sparse).

  lpunpack.py SUPER.raw NAME OUT.img      (NAME e.g. vendor; lists the partitions when NAME is "-")

Format: AOSP system/core/fs_mgr/liblp (metadata_format.h): geometry at 4096 (magic 0x616c4467), the
primary metadata at 4096 + 2 * 4096 (header magic 0x414c5030), tables of partitions (52 bytes: name[36],
attributes, first_extent_index, num_extents, group_index) and extents (24 bytes: num_sectors,
target_type, target_data = physical sector, target_source).
"""
import struct, sys

path, want, out = sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None
f = open(path, 'rb')
f.seek(4096)
geo = f.read(52)
magic, struct_size = struct.unpack_from('<II', geo, 0)
assert magic == 0x616C4467, hex(magic)
meta_max = struct.unpack_from('<I', geo, 40)[0]
f.seek(4096 + 2 * 4096)
hdr = f.read(256)
assert struct.unpack_from('<I', hdr, 0)[0] == 0x414C5030, 'metadata header'
header_size = struct.unpack_from('<I', hdr, 8)[0]
tables_size = struct.unpack_from('<I', hdr, 44)[0]
# table descriptors after: header_checksum[32] + tables_size + tables_checksum[32]
off = 12 + 32 + 4 + 32
descs = [struct.unpack_from('<III', hdr, off + 12 * i) for i in range(4)]   # partitions, extents, groups, block_devices
f.seek(4096 + 2 * 4096 + header_size)
tables = f.read(tables_size)
(p_off, p_n, p_sz), (e_off, e_n, e_sz) = descs[0], descs[1]
extents = [struct.unpack_from('<QIQI', tables, e_off + i * e_sz) for i in range(e_n)]
for i in range(p_n):
    raw = tables[p_off + i * p_sz:p_off + (i + 1) * p_sz]
    name = raw[:36].rstrip(b'\0').decode()
    attrs, first, num, group = struct.unpack_from('<IIII', raw, 36)
    size = sum(extents[first + k][0] for k in range(num)) * 512
    if want == '-':
        print(f'{name:24} {size / 1048576:9.1f} MiB  {num} extents')
        continue
    if name != want:
        continue
    with open(out, 'wb') as o:
        for k in range(num):
            sectors, ttype, data, src = extents[first + k]
            if ttype != 0:      # 0 = linear; 1 = zero
                o.write(b'\0' * sectors * 512)
                continue
            f.seek(data * 512)
            left = sectors * 512
            while left:
                chunk = f.read(min(left, 64 << 20))
                o.write(chunk)
                left -= len(chunk)
    print(f'{name}: {size} bytes -> {out}')
