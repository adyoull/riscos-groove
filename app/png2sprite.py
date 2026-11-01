#!/usr/bin/env python3
"""Make a RISC OS sprite file (filetype &FF9) from PNG pictures.

   package/png2sprite.py OUT,ff9 NAME=PICTURE.png [NAME=PICTURE.png...]

Each picture becomes one sprite called NAME (lower case, up to 12
characters), in the 32 bits per pixel format with a 1 bit per pixel mask
(pixels with alpha of 128 or more are solid). Sprites in this format need
RISC OS 3.5 or later. The size in pixels is the picture's, at 90 dpi:
34 x 34 pixels is the usual size of an application's icon (!app), and
17 x 17 the small one (sm!app).

The PNG must be 8 bits per channel, not interlaced: RGB, RGBA, grey or
grey with alpha (most editors save this). Standard library only.
"""
import struct, sys, zlib

SPRITE_TYPE_32BPP = 6
DPI = 90


def read_png(path):
    """Return (width, height, rows of (r, g, b, a) tuples)."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos, idat, ihdr = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
    w, h, depth, colour, _, _, interlace = ihdr
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour)
    if depth != 8 or channels is None or interlace:
        raise ValueError(f'{path}: needs 8 bits per channel, RGB(A) or grey(+alpha), not interlaced')
    raw = zlib.decompress(idat)
    stride = w * channels
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        px = []
        for x in range(w):
            v = line[x * channels:(x + 1) * channels]
            if channels == 1:
                px.append((v[0], v[0], v[0], 255))
            elif channels == 2:
                px.append((v[0], v[0], v[0], v[1]))
            elif channels == 3:
                px.append((v[0], v[1], v[2], 255))
            else:
                px.append(tuple(v))
        rows.append(px)
    return w, h, rows


def sprite(name, w, h, rows):
    name = name.lower()
    if not 0 < len(name) <= 12:
        raise ValueError(f'sprite name {name!r}: 1 to 12 characters')
    image = b''.join(struct.pack('<I', r | g << 8 | b << 16) for row in rows for r, g, b, _ in row)
    mask_words = (w + 31) // 32
    mask = b''
    for row in rows:
        bits = [0] * mask_words
        for x, (_, _, _, a) in enumerate(row):
            if a >= 128:
                bits[x // 32] |= 1 << (x % 32)   # leftmost pixel is bit 0
        mask += struct.pack(f'<{mask_words}I', *bits)
    mode = SPRITE_TYPE_32BPP << 27 | DPI << 14 | DPI << 1 | 1
    header_size = 44
    size = header_size + len(image) + len(mask)
    return struct.pack('<I12s7I', size, name.encode('latin-1'), w - 1, h - 1, 0, 31,
                       header_size, header_size + len(image), mode) + image + mask


def main(argv):
    if len(argv) < 3 or any('=' not in a for a in argv[2:]):
        sys.exit(__doc__)
    sprites = b''
    for arg in argv[2:]:
        name, path = arg.split('=', 1)
        w, h, rows = read_png(path)
        sprites += sprite(name, w, h, rows)
    n = len(argv) - 2
    # A sprite file is a sprite area without its first word (the area size):
    # offsets count from the start of the area, so 4 more than in the file.
    with open(argv[1], 'wb') as f:
        f.write(struct.pack('<3I', n, 16, 16 + len(sprites)) + sprites)


if __name__ == '__main__':
    main(sys.argv)
