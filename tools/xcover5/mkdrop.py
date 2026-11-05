#!/usr/bin/env python3
"""Pack files for the XCover5's CACHE drop zone (bsd/dev/xc5_drop.c).

  mkdrop.py OUT.tar DEST=SRC [DEST=SRC ...]
  mkdrop.py OUT.tar --mesa DIR      (the mesa-ios artifact: lib/*.dylib, osmesa-test)

DEST is relative to /private/var/xc5/ (parent directories are added). OUT.tar
is an Odin AP tar holding cache.img; flashing it overwrites the CACHE
partition (p34, 200 MiB; Android does not use it on this phone). The kernel
copies the files into the root once launchd remounts it read-write, once per
package (the id is the packing time).
"""
import argparse
import io
import os
import struct
import sys
import tarfile
import time
from pathlib import Path

PREFIX = '/private/var/xc5/'
HEADER = 4096
LIMIT = 192 << 20


def pack(items):
    entries, dirs = [], set()
    for dest, src in items:
        dest = dest.strip('/')
        parts = dest.split('/')
        for i in range(1, len(parts)):
            d = PREFIX + '/'.join(parts[:i])
            if d not in dirs:
                dirs.add(d)
                entries.append((d, 0, b''))
        data = Path(src).read_bytes()
        mode = 0o755 if os.access(src, os.X_OK) or dest.endswith('.dylib') else 0o644
        entries.append((PREFIX + dest, mode, data))
    payload = bytearray()
    for path, mode, data in entries:
        p = path.encode()
        payload += struct.pack('<IIQ', len(p), mode, len(data)) + p
        payload += b'\0' * (-len(payload) % 8)
        payload += data
        payload += b'\0' * (-len(payload) % 8)
    if len(payload) > LIMIT:
        sys.exit(f'package too large: {len(payload)} bytes (CACHE limit {LIMIT})')
    ident = int(time.time())
    header = struct.pack('<8sIIQQ', b'XC5DROP1', 1, len(entries), len(payload), ident)
    blob = header + b'\0' * (HEADER - len(header)) + bytes(payload)
    blob += b'\0' * (-len(blob) % 4096)
    return blob, entries, ident


def sparse(raw):
    """Android sparse image: S-Boot refused a raw cache.img ("Complete(Write)
    operation failed" after cache.img). One RAW chunk of 4 KiB blocks."""
    blocks = len(raw) // 4096
    header = struct.pack('<IHHHHIIII', 0xed26ff3a, 1, 0, 28, 12, 4096, blocks, 1, 0)
    chunk = struct.pack('<HHII', 0xcac1, 0, blocks, 12 + len(raw))
    return header + chunk + raw


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out')
    ap.add_argument('items', nargs='*', help='DEST=SRC')
    ap.add_argument('--mesa', help='mesa-ios artifact directory')
    args = ap.parse_args()
    items = [i.split('=', 1) for i in args.items]
    if args.mesa:
        mesa = Path(args.mesa)
        items += [(f'lib/{p.name}', str(p)) for p in sorted((mesa / 'lib').glob('*.dylib'))]
        if (mesa / 'osmesa-test').exists():
            items.append(('osmesa-test', str(mesa / 'osmesa-test')))
    if not items:
        ap.error('nothing to pack')
    blob, entries, ident = pack(items)
    blob = sparse(blob)
    with tarfile.open(args.out, 'w', format=tarfile.USTAR_FORMAT) as t:
        info = tarfile.TarInfo('cache.img')
        info.size = len(blob)
        info.mtime = ident
        info.mode = 0o644
        t.addfile(info, fileobj=io.BytesIO(blob))
    for path, mode, data in entries:
        print(f'  {"dir " if mode == 0 else oct(mode)} {path} {len(data)} bytes')
    print(f'{args.out}: Odin AP tar with cache.img, package {ident}, {len(blob)} bytes')


if __name__ == '__main__':
    main()
