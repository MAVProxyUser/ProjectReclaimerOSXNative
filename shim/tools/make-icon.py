#!/usr/bin/env python3
"""Draw Reclaimer.app's icon (an original design, no game artwork): shim/assets/Reclaimer.icns.

    python3 shim/tools/make-icon.py        # needs Pillow; iconutil comes with macOS

The .icns is checked in, so Macs without Pillow use it as is (shim/tools/make-app.sh copies it into the app).
"""
import math, os, shutil, subprocess, sys, tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'assets', 'Reclaimer.icns')
S = 1024


def font(size):
    for p in ('/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf', '/System/Library/Fonts/Supplemental/Arial Bold.ttf',
              '/System/Library/Fonts/Helvetica.ttc', '/Library/Fonts/Arial Bold.ttf'):
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def draw():
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    # the macOS icon grid: an 824 px rounded square centred on the 1024 canvas, with a soft shadow
    box = (100, 100, 924, 924)
    shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ImageDraw.Draw(shadow).rounded_rectangle((100, 118, 924, 942), 185, fill=(0, 0, 0, 120))
    img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(18)))
    bg = Image.new('RGBA', (S, S)); px = bg.load()
    for y in range(S):   # dark slate -> deep teal, top to bottom
        t = y / (S - 1); px_row = (int(22 + 8 * t), int(30 + 40 * t), int(44 + 36 * t), 255)
        for x in range(S): px[x, y] = px_row
    mask = Image.new('L', (S, S), 0); ImageDraw.Draw(mask).rounded_rectangle(box, 185, fill=255)
    img.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # the "reclaim" arc: 300 degrees of a green ring with an arrowhead at its end
    c, r, w = (512, 512), 285, 58
    d.arc((c[0] - r, c[1] - r, c[0] + r, c[1] + r), start=-60, end=240, fill=(120, 220, 110, 255), width=w)
    a = math.radians(-60); tip = (c[0] + r * math.cos(a), c[1] + r * math.sin(a))
    tan = (math.cos(a - math.pi / 2), math.sin(a - math.pi / 2)); nrm = (math.cos(a), math.sin(a))
    head = [(tip[0] + nrm[0] * 70, tip[1] + nrm[1] * 70), (tip[0] - nrm[0] * 70, tip[1] - nrm[1] * 70), (tip[0] - tan[0] * 95, tip[1] - tan[1] * 95)]
    d.polygon(head, fill=(120, 220, 110, 255))
    # the R
    f = font(330); t = 'R'
    bb = d.textbbox((0, 0), t, font=f); tw, th = bb[2] - bb[0], bb[3] - bb[1]
    d.text((c[0] - tw / 2 - bb[0], c[1] - th / 2 - bb[1] - 8), t, font=f, fill=(236, 242, 246, 255))
    return img


def main():
    img = draw()
    tmp = tempfile.mkdtemp(); iconset = os.path.join(tmp, 'Reclaimer.iconset'); os.makedirs(iconset)
    for base in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            n = base * scale; name = 'icon_%dx%d%s.png' % (base, base, '@2x' if scale == 2 else '')
            img.resize((n, n), Image.LANCZOS).save(os.path.join(iconset, name))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', OUT], check=True)
    img.save(os.path.join(tmp, 'preview.png')); shutil.copy(os.path.join(tmp, 'preview.png'), os.path.join(os.path.dirname(OUT), 'Reclaimer-1024.png'))
    shutil.rmtree(tmp)
    print('wrote', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
