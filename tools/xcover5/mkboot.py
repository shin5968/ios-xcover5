#!/usr/bin/env python3
"""Build a Galaxy XCover5 BOOT image (Android boot header v2) for S-Boot.

Takes the header fields, cmdline, DTB section and AVB vbmeta blob from a
stock (or Magisk-patched) boot.img of the same phone and replaces the kernel
with our loader Image (gzip'd, like the stock kernel) and the ramdisk with
RAMDISK (a tools/mkpayload.py payload, or any file for a probe-only boot).

Layout, as Magisk writes it on this phone: pages of header / kernel /
ramdisk / dtb, "SEANDROIDENFORCE", the stock vbmeta blob at the next 4 KiB,
zero padding, AVB footer in the last 64 bytes of the partition. The hash in
vbmeta no longer matches; an unlocked S-Boot boots it anyway (orange state),
exactly like a Magisk boot image.

Usage: mkboot.py STOCK_BOOT.img LOADER_Image RAMDISK OUT.img [--tar OUT.tar]
--tar also writes an Odin-flashable tar containing boot.img (AP slot).
"""
import argparse
import gzip
import io
import hashlib
import struct
import tarfile
import time
from pathlib import Path

HDR_FMT = '<8s10I16s512s32s1024sIQIIQ'
V2_HEADER_SIZE = 0x67c


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('stock', type=Path)
    ap.add_argument('image', type=Path)
    ap.add_argument('ramdisk', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--tar', type=Path)
    ap.add_argument('--touch-fw', type=Path,
                    help='Novatek firmware (vendor/firmware/nt36525_xcover5.bin) for the '
                         'loader to download at boot; appended after the ramdisk')
    a = ap.parse_args()

    stock = a.stock.read_bytes()
    h = list(struct.unpack_from(HDR_FMT, stock, 0))
    (magic, ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, hver, osver,
     name, cmdline, _id, xcmd, rdtbo_sz, rdtbo_off, hsz, dtbsz, dtbaddr) = h
    if magic != b'ANDROID!' or hver != 2 or hsz != V2_HEADER_SIZE:
        raise SystemExit(f'{a.stock}: not a boot header v2 image (version {hver})')
    if ssz or rdtbo_sz:
        raise SystemExit('stock image has a second stage or recovery dtbo; unexpected')

    def pages(n):
        return (n + page - 1) // page * page

    off = page + pages(ksz) + pages(rsz)
    dtb = stock[off:off + dtbsz]
    if dtb[:4] != bytes.fromhex('d7b7ab1e'):
        raise SystemExit('stock dtb section is not an Exynos dt_table')
    fmagic, _maj, _min, _orig, vb_off, vb_size = struct.unpack_from('>4sIIQQQ', stock, len(stock) - 64)
    if fmagic != b'AVBf':
        raise SystemExit('stock image has no AVB footer; use a full dump of the BOOT partition')
    vbmeta = stock[vb_off:vb_off + vb_size]
    part_size = len(stock)

    image = a.image.read_bytes()
    if image[0x38:0x3c] != b'ARM\x64':
        raise SystemExit(f'{a.image}: no arm64 Image header')
    kernel = gzip.compress(image, mtime=0)
    ramdisk = a.ramdisk.read_bytes()
    if a.touch_fw:
        # shim/xcover5/loader.c: FW bytes, "NVTFWXC5", u32 size, u32 0 at the end.
        fw = a.touch_fw.read_bytes()
        ramdisk += fw + b'NVTFWXC5' + struct.pack('<II', len(fw), 0)

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b'', b'', dtb):	# kernel ramdisk second recovery_dtbo dtb
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))
    digest = sha.digest() + b'\0' * 12

    hdr = struct.pack(HDR_FMT, magic, len(kernel), kaddr, len(ramdisk), raddr, 0, saddr, tags,
                      page, hver, osver, name, cmdline, digest, xcmd, 0, 0, hsz, len(dtb), dtbaddr)
    body = bytearray(hdr + b'\0' * (page - len(hdr)))
    for blob in (kernel, ramdisk, dtb):
        body += blob + b'\0' * (pages(len(blob)) - len(blob))
    body += b'SEANDROIDENFORCE'
    orig = len(body)
    vb_new = (orig + 4095) // 4096 * 4096
    if vb_new + len(vbmeta) > part_size - 64:
        raise SystemExit(f'image too large: {vb_new + len(vbmeta) + 64} bytes > BOOT partition '
                         f'{part_size} (ramdisk {len(ramdisk)} bytes)')
    out = bytearray(part_size)
    out[:orig] = body
    out[vb_new:vb_new + len(vbmeta)] = vbmeta
    out[part_size - 64:] = struct.pack('>4sIIQQQ28x', b'AVBf', 1, 0, orig, vb_new, len(vbmeta))
    a.out.write_bytes(out)
    print(f'{a.out}: {part_size} bytes (loader {len(image)} -> gz {len(kernel)}, '
          f'ramdisk {len(ramdisk)}, dtb {len(dtb)}, free {part_size - 64 - vb_new - len(vbmeta)})')

    if a.tar:
        with tarfile.open(a.tar, 'w', format=tarfile.USTAR_FORMAT) as t:
            info = tarfile.TarInfo('boot.img')
            info.size = len(out)
            info.mtime = int(time.time())
            info.mode = 0o644
            t.addfile(info, fileobj=io.BytesIO(bytes(out)))
        print(f'{a.tar}: Odin AP tar with boot.img')


if __name__ == '__main__':
    main()
