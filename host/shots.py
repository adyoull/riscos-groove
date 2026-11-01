#!/usr/bin/env python3
"""Turn the drawing logs from host/test_ui.c (via fake_ro.c) into PNGs.

Each log is the exact sequence of OS_Plot, ColourTrans and Wimp_TextOp
calls Groove's redraw code made, window by window, bottom to top. This
replays them on a 1920x1080 "EX1 EY1" screen (2 OS units per pixel), adds
plain RISC OS-style window furniture and an icon bar, and writes a PNG.
It shows what the drawing code draws; it is not a capture from a Pi (the
real desktop font, furniture and tool sprites will differ).

usage: shots.py LOG... OUTDIR
"""
import math, os, sys
from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
FONT = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 14)
FONT_B = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 13)


def word_rgb(w):            # &BBGGRR00 -> (r, g, b)
    w = int(w, 16)
    return ((w >> 8) & 255, (w >> 16) & 255, (w >> 24) & 255)


def px(x, y):
    return x // 2, H - 1 - y // 2


class Painter:
    def __init__(self, base):
        self.base = base
        self.layer = None
        self.clip = (0, 0, W, H)
        self.col = (0, 0, 0)

    def begin(self, clip):
        self.flush()
        self.clip = clip
        self.layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.layer)

    def flush(self):
        if self.layer is None:
            return
        x0, y0, x1, y1 = [max(0, min(v, m)) for v, m in zip(self.clip, (W, H, W, H))]
        if x1 > x0 and y1 > y0:
            box = (x0, y0, x1, y1)
            part = self.layer.crop(box)
            self.base.paste(part, box[:2], part)
        self.layer = None

    def os_clip(self, x0, y0, x1, y1):       # inclusive OS units
        a, b2 = px(x0, y1), px(x1, y0)
        self.begin((a[0], a[1], b2[0] + 1, b2[1] + 1))


def furniture(d, x0, y0, x1, y1, flags, title):
    """Plain RISC OS 5-ish window furniture around a visible area (pixels)."""
    t = 20
    grey, dark, light = (221, 221, 221), (119, 119, 119), (238, 238, 238)
    has = lambda bit: flags & (1 << bit)
    right = x1 + (20 if has(28) else 0)
    if has(26):
        d.rectangle((x0 - 1, y0 - t - 1, right, y0 - 1), fill=grey, outline=(0, 0, 0))
        bx = x0
        for bit, glyph in ((24, 'back'), (25, 'close')):
            if has(bit):
                d.rectangle((bx, y0 - t, bx + t - 1, y0 - 2), fill=light, outline=dark)
                if glyph == 'close':
                    d.line((bx + 6, y0 - t + 6, bx + t - 7, y0 - 8), fill=(0, 0, 0), width=2)
                    d.line((bx + 6, y0 - 8, bx + t - 7, y0 - t + 6), fill=(0, 0, 0), width=2)
                else:
                    d.rectangle((bx + 5, y0 - t + 5, bx + 12, y0 - 9), outline=(0, 0, 0))
                    d.rectangle((bx + 8, y0 - t + 8, bx + 15, y0 - 6), fill=light, outline=(0, 0, 0))
                bx += t
        if has(27):
            d.rectangle((right - t, y0 - t, right - 1, y0 - 2), fill=light, outline=dark)
            d.rectangle((right - t + 5, y0 - t + 5, right - 6, y0 - 7), outline=(0, 0, 0))
        tw = d.textlength(title, font=FONT_B)
        d.text(((bx + right - (t if has(27) else 0)) / 2 - tw / 2, y0 - 5), title, fill=(0, 0, 0),
               font=FONT_B, anchor='ls')
    if has(28):
        d.rectangle((x1, y0, right, y1), fill=(190, 190, 190), outline=(0, 0, 0))
        d.rectangle((x1 + 1, y0 + 1, right - 1, y0 + 19), fill=light, outline=dark)
        d.polygon(((x1 + 10, y0 + 5), (x1 + 4, y0 + 14), (x1 + 16, y0 + 14)), fill=(0, 0, 0))
        bot = y1 - (20 if has(29) else 0)
        d.rectangle((x1 + 1, bot - 20, right - 1, bot - 1), fill=light, outline=dark)
        d.polygon(((x1 + 10, bot - 5), (x1 + 4, bot - 14), (x1 + 16, bot - 14)), fill=(0, 0, 0))
        d.rectangle((x1 + 3, y0 + 24, right - 3, y0 + 24 + max(30, (y1 - y0) // 2)), fill=(150, 150, 150))
        if has(29):
            d.rectangle((x1 + 1, y1 - 19, right - 1, y1 - 1), fill=light, outline=dark)
            d.line((x1 + 5, y1 - 4, right - 4, y1 - 15), fill=(0, 0, 0))
    d.rectangle((x0 - 1, y0 - 1, right, y1), outline=(0, 0, 0))


def iconbar(d):
    d.rectangle((0, H - 48, W, H), fill=(221, 221, 221), outline=(119, 119, 119))
    for x, label, col in ((40, 'Apps', (90, 140, 210)), (130, 'SDFS::RISCOSpi', (150, 150, 150))):
        d.rectangle((x - 14, H - 44, x + 14, H - 20), fill=col, outline=(60, 60, 60))
        d.text((x, H - 6), label, fill=(0, 0, 0), font=FONT, anchor='ms')
    gx = W - 60                     # Groove's icon: the app's own colours
    d.rounded_rectangle((gx - 16, H - 46, gx + 16, H - 18), 6, fill=(38, 39, 43))
    for i, (h, c) in enumerate(((10, (232, 197, 71)), (18, (91, 191, 106)), (14, (74, 144, 226)), (22, (229, 72, 77)))):
        d.rectangle((gx - 12 + i * 7, H - 22 - h, gx - 7 + i * 7, H - 22), fill=c)
    d.text((gx, H - 6), 'Groove', fill=(0, 0, 0), font=FONT, anchor='ms')


def render(log, out):
    base = Image.new('RGBA', (W, H), (118, 136, 154, 255))
    bd = ImageDraw.Draw(base)
    iconbar(bd)
    p = Painter(base)
    for line in open(log):
        parts = line.rstrip('\n').split(' ')
        k = parts[0]
        if k == 'W':
            p.flush()
            x0, y0, x1, y1, pane, flags = map(int, parts[1:7])
            title = ' '.join(parts[7:])
            a, b = px(x0, y1), px(x1, y0)
            if not pane:
                furniture(ImageDraw.Draw(base), a[0], a[1], b[0] + 1, b[1] + 1, flags, title)
        elif k == 'E':
            p.flush()
        elif k == 'G':
            p.os_clip(*map(int, parts[1:5]))
        elif k == 'C':
            p.col = word_rgb(parts[1])
        elif p.layer is None:
            continue
        elif k == 'R':
            x0, y0, x1, y1 = map(int, parts[1:5])
            a, b = px(min(x0, x1), max(y0, y1)), px(max(x0, x1), min(y0, y1))
            p.d.rectangle((a[0], a[1], b[0], b[1]), fill=p.col)
        elif k == 'T':
            pts = list(map(int, parts[1:7]))
            p.d.polygon([px(pts[i], pts[i + 1]) for i in (0, 2, 4)], fill=p.col)
        elif k == 'O':
            cx, cy, x, y = map(int, parts[1:5])
            r = math.hypot(x - cx, y - cy) / 2
            c = px(cx, cy)
            p.d.ellipse((c[0] - r, c[1] - r, c[0] + r, c[1] + r), fill=p.col)
        elif k == 'L':
            x0, y0, x1, y1 = map(int, parts[1:5])
            p.d.line((px(x0, y0), px(x1, y1)), fill=p.col, width=1)
        elif k == 'X':
            x, y = int(parts[1]), int(parts[2])
            text = ' '.join(parts[5:])
            p.d.text(px(x, y), text, fill=word_rgb(parts[3]), font=FONT, anchor='ls')
    p.flush()
    base.convert('RGB').save(out)
    print(out)


for log in sys.argv[1:-1]:
    render(log, os.path.join(sys.argv[-1], os.path.basename(log)[:-4] + '.png'))
