#!/usr/bin/env python3
"""The drop zone's default wallpaper (gpu/eagl/src/xc5_sb.c, boot-arg xc5wall=1): a gradient of our own,
after wardevsa/ios-a5xelte herolte's tools/herolte-make-wallpaper.py, at the iPhone X's 1125x2436.

  mkwallpaper.py OUT.png [--from PICTURE]

With --from, the user's picture is fitted (cropped to fill) to 1125x2436 instead.
"""
import argparse
import math

from PIL import Image, ImageFilter

W, H = 1125, 2436


def gradient():
    small = Image.new('RGB', (W // 4, H // 4))
    p = small.load()
    w, h = small.size
    for y in range(h):
        for x in range(w):
            t, a = y / h, x / w
            r = int(25 + 60 * t + 60 * math.sin(a * 3 + t * 2) ** 2)
            g = int(20 + 40 * (1 - t) + 50 * math.sin(t * 4 + a) ** 2)
            b = int(90 + 120 * (1 - t) + 40 * math.cos(a * 2 - t * 3) ** 2)
            p[x, y] = (min(r, 255), min(g, 255), min(b, 255))
    return small.resize((W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(6))


def fitted(path):
    im = Image.open(path).convert('RGB')
    scale = max(W / im.width, H / im.height)
    im = im.resize((round(im.width * scale), round(im.height * scale)), Image.LANCZOS)
    left, top = (im.width - W) // 2, (im.height - H) // 2
    return im.crop((left, top, left + W, top + H))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--from', dest='src')
    args = ap.parse_args()
    im = fitted(args.src) if args.src else gradient()
    im.save(args.out, optimize=True)
    print(f'{args.out}: {im.width}x{im.height}')


if __name__ == '__main__':
    main()
