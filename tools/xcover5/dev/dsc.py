#!/usr/bin/env python3
"""Symbolise console crash addresses against the iOS 16.7.16 dyld shared cache.

  dsc.py SLIDE ADDR...      (slid addresses as CRASH lines print them; SLIDE from the same CRASH line)

Prints the image, offset, nearest exported symbol and the function start (LC_FUNCTION_STARTS).
Exported symbols only: ObjC methods show as "nearest export + big offset"; use the function start and
disassemble (dsdis.sh) or read the selector strings around (dsstr.py). Needs dev/mntsys.sh first.
Also importable: read(addr, n), image_info(base).
"""
import bisect, os, struct, sys

D = '/home/user/xc5/sysos/root/System/Library/Caches/com.apple.dyld'
files = []
for n in sorted(os.listdir(D)):
    if not n.startswith('dyld_shared_cache_arm64') or n.endswith('.symbols') or n.endswith('.map'):
        continue
    f = open(os.path.join(D, n), 'rb')
    h = f.read(0x200)
    if not h.startswith(b'dyld_v1'):
        continue
    mo, mc = struct.unpack_from('<II', h, 0x10)
    f.seek(mo)
    maps = [struct.unpack_from('<QQQII', f.read(32)) for _ in range(mc)]
    files.append((n, f, h, maps))
main = [x for x in files if x[0] == 'dyld_shared_cache_arm64'][0]


def read(addr, n):
    for _, f, _, maps in files:
        for a, size, off, _, _ in maps:
            if a <= addr < a + size:
                f.seek(off + addr - a)
                return f.read(n)
    return None


h = main[2]
io_, ic = struct.unpack_from('<II', h, 0x1c0)
if ic == 0:
    io_, ic = struct.unpack_from('<II', h, 0x18)
images = []
for i in range(ic):
    main[1].seek(io_ + 32 * i)
    a, _, _, po, _ = struct.unpack('<QQQII', main[1].read(32))
    main[1].seek(po)
    images.append((a, main[1].read(300).split(b'\0')[0].decode()))
images.sort()


def uleb(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7f) << s; s += 7
        if c < 0x80:
            return r, i


def trie(b):
    out, stack = [], [(0, '')]
    while stack:
        node, pre = stack.pop()
        size, i = uleb(b, node)
        if size:
            flags, j = uleb(b, i)
            if not flags & 8:
                off, j = uleb(b, j)
                out.append((off, pre))
        i += size
        nch = b[i]; i += 1
        for _ in range(nch):
            e = b.index(b'\0', i)
            s = b[i:e].decode(errors='replace'); i = e + 1
            nx, i = uleb(b, i)
            stack.append((nx, pre + s))
    return out


_cache = {}


def image_info(base):
    if base in _cache:
        return _cache[base]
    mh = read(base, 0x8000)
    ncmds = struct.unpack_from('<I', mh, 0x10)[0]
    p, segs, exp, fst = 0x20, {}, None, None
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', mh, p)
        if cmd == 0x19:
            name = mh[p + 8:p + 24].split(b'\0')[0].decode()
            va, vs, fo, fs = struct.unpack_from('<QQQQ', mh, p + 24)
            segs[name] = (va, vs, fo)
        elif cmd == 0x80000033:
            exp = struct.unpack_from('<II', mh, p + 8)
        elif cmd == 0x26:
            fst = struct.unpack_from('<II', mh, p + 8)
        p += sz
    le, syms, starts = segs.get('__LINKEDIT'), [], []
    if exp and le:
        b = read(le[0] + exp[0] - le[2], exp[1])
        if b:
            syms = sorted(trie(b))
    if fst and le:
        b = read(le[0] + fst[0] - le[2], fst[1])
        a, i = segs['__TEXT'][0], 0
        while b and i < len(b):
            v, i = uleb(b, i)
            if v == 0:
                break
            a += v
            starts.append(a)
    _cache[base] = (segs, syms, starts)
    return _cache[base]


def symbolise(sa, slide):
    a = sa - slide
    k = bisect.bisect_right([x[0] for x in images], a) - 1
    for j in range(max(0, k - 3), min(len(images), k + 2))[::-1]:
        base, path = images[j]
        segs, syms, starts = image_info(base)
        t = segs.get('__TEXT')
        if t and t[0] <= a < t[0] + t[1]:
            off = a - base
            i = bisect.bisect_right([s[0] for s in syms], off) - 1
            sym = f'{syms[i][1]}+{off - syms[i][0]:#x}' if i >= 0 else '?'
            fi = bisect.bisect_right(starts, a) - 1
            fs = f'func {starts[fi] - base:#x}+{a - starts[fi]:#x}' if fi >= 0 else ''
            return f'{sa:#x} ({a:#x}): {os.path.basename(path)} +{off:#x}  {sym}  [{fs}]'
    return f'{sa:#x} ({a:#x}): not in any image __TEXT'


if __name__ == '__main__':
    slide = int(sys.argv[1], 16)
    for s in sys.argv[2:]:
        print(symbolise(int(s, 16), slide))
