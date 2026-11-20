#!/usr/bin/env python3
"""Make engine/glyphs.c: the music symbols Groove's Score window draws,
as outlines taken from the Bravura font (SIL Open Font Licence 1.1; see
app/Licences/Bravura). Run: python3 tools/mkglyphs.py Bravura.otf

Each glyph is a path in font units (1000 to the em; a staff space is 250),
y upwards, with the origin where SMuFL puts it (on the staff line the
symbol sits on), as moves, lines and cubic curves."""
import sys
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.boundsPen import BoundsPen

GLYPHS = [  # (C name, SMuFL code point)
    ("G_CLEF", 0xE050), ("F_CLEF", 0xE062), ("PERC_CLEF", 0xE069), ("BRACE", 0xE000),
    ("HEAD_WHOLE", 0xE0A2), ("HEAD_HALF", 0xE0A3), ("HEAD_BLACK", 0xE0A4),
    ("HEAD_X_WHOLE", 0xE0A7), ("HEAD_X_HALF", 0xE0A8), ("HEAD_X", 0xE0A9),
    ("HEAD_CIRCLE_X", 0xE0B3), ("HEAD_DIAMOND", 0xE0DB), ("HEAD_TRIANGLE_DOWN", 0xE0C7),
    ("HEAD_TRIANGLE_UP", 0xE0BE), ("HEAD_SLASHED", 0xE0CF),
    ("FLAT", 0xE260), ("NATURAL", 0xE261), ("SHARP", 0xE262), ("DOUBLE_SHARP", 0xE263), ("DOUBLE_FLAT", 0xE264),
    ("REST_WHOLE", 0xE4E3), ("REST_HALF", 0xE4E4), ("REST_QUARTER", 0xE4E5), ("REST_8TH", 0xE4E6),
    ("REST_16TH", 0xE4E7), ("REST_32ND", 0xE4E8), ("REST_64TH", 0xE4E9), ("REST_128TH", 0xE4EA),
    ("FLAG_8TH_UP", 0xE240), ("FLAG_8TH_DOWN", 0xE241), ("FLAG_16TH_UP", 0xE242), ("FLAG_16TH_DOWN", 0xE243),
    ("FLAG_32ND_UP", 0xE244), ("FLAG_32ND_DOWN", 0xE245), ("FLAG_64TH_UP", 0xE246), ("FLAG_64TH_DOWN", 0xE247),
    ("FLAG_128TH_UP", 0xE248), ("FLAG_128TH_DOWN", 0xE249),
    ("DOT", 0xE1E7),
] + [("TIME_%d" % d, 0xE080 + d) for d in range(10)] + [("TUPLET_%d" % d, 0xE880 + d) for d in range(10)] + [
    ("MET_QUARTER", 0xECA5),
    ("STACCATO_ABOVE", 0xE4A2), ("STACCATO_BELOW", 0xE4A3), ("ACCENT_ABOVE", 0xE4A0), ("ACCENT_BELOW", 0xE4A1),
    ("DYN_PPP", 0xE52A), ("DYN_PP", 0xE52B), ("DYN_P", 0xE520), ("DYN_MP", 0xE52C), ("DYN_MF", 0xE52D),
    ("DYN_F", 0xE522), ("DYN_FF", 0xE52F), ("DYN_FFF", 0xE530),
    ("PED", 0xE650), ("PED_UP", 0xE655),
    ("G_CLEF_8VB", 0xE052), ("C_CLEF", 0xE05C),
    ("G_CLEF_SMALL", 0xE07A), ("C_CLEF_SMALL", 0xE07B), ("F_CLEF_SMALL", 0xE07C),
    ("OTTAVA_8VA", 0xE511), ("OTTAVA_8VB", 0xE51C),
    ("QUINDICESIMA_A", 0xE515), ("QUINDICESIMA_B", 0xE517),
    ("ARPEGGIATO", 0xE63C), ("ARROWHEAD_UP", 0xEB78), ("ARROWHEAD_DOWN", 0xEB7C),
]

def main(path, out):
    f = TTFont(path)
    cmap = f.getBestCmap()
    gs = f.getGlyphSet()
    upm = f["head"].unitsPerEm
    assert upm == 1000, upm
    data, table = [], []
    for name, cp in GLYPHS:
        g = cmap.get(cp)
        if g is None:
            sys.exit("Bravura has no U+%04X (%s)" % (cp, name))
        rp, bp = RecordingPen(), BoundsPen(gs)
        gs[g].draw(rp)
        gs[g].draw(bp)
        start = len(data)
        for op, args in rp.value:
            if op == "moveTo":
                data += [1, *map(round, args[0])]
            elif op == "lineTo":
                data += [2, *map(round, args[0])]
            elif op == "curveTo":
                for i in range(0, len(args) - 2, 3) if len(args) > 3 else [0]:
                    pass
                assert len(args) == 3, (name, op, args)
                data += [3] + [round(v) for p in args for v in p]
            elif op in ("closePath", "endPath"):
                data += [4]
            else:
                sys.exit("unexpected %s in %s" % (op, name))
        b = bp.bounds or (0, 0, 0, 0)
        adv = gs[g].width
        table.append((name, cp, start, len(data) - start, [round(v) for v in b], adv))
    with open(out, "w") as o:
        o.write("/* Groove - music symbols for the Score window: outlines from the Bravura\n"
                "   font by Steinberg Media Technologies, under the SIL Open Font Licence 1.1\n"
                "   (app/Licences/Bravura). Made by tools/mkglyphs.py; don't edit.\n\n"
                "   Font units: 1000 to the em, a staff space is 250, y upwards. Ops: 1 move\n"
                "   x y, 2 line x y, 3 curve x1 y1 x2 y2 x3 y3, 4 close. */\n"
                "#include \"glyphs.h\"\n\n")
        o.write("const short gr_glyph_data[%d] = {\n" % len(data))
        for i in range(0, len(data), 16):
            o.write("  " + ", ".join(str(v) for v in data[i:i + 16]) + ",\n")
        o.write("};\n\nconst gr_glyph gr_glyphs[GL_COUNT] = {\n")
        for name, cp, start, n, b, adv in table:
            o.write("  { %d, %d, %d, %d, %d, %d, %d },   /* %s U+%04X */\n" % (start, n, b[0], b[1], b[2], b[3], adv, name, cp))
        o.write("};\n")
    with open(out.replace(".c", ".h"), "w") as o:
        o.write("/* Groove - the Score window's music symbols (glyphs.c, made by\n"
                "   tools/mkglyphs.py from Bravura; SIL Open Font Licence 1.1). */\n"
                "#ifndef GLYPHS_H\n#define GLYPHS_H\n\n"
                "typedef struct { unsigned short start, len; short x0, y0, x1, y1, adv; } gr_glyph;\n\n"
                "enum {\n")
        for name, cp, *_ in table:
            o.write("  GL_%s,\n" % name)
        o.write("  GL_COUNT\n};\n\nextern const short gr_glyph_data[];\nextern const gr_glyph gr_glyphs[GL_COUNT];\n\n#endif\n")
    print(len(table), "glyphs,", len(data), "shorts")

main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "engine/glyphs.c")
