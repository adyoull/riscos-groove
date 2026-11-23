#!/usr/bin/env python3
"""Check (--check) that a static library (ar archive of ELF32 little-endian
objects) has its data sections all at least word-aligned, or make a copy
where they are.

Why: RISC OS runs with alignment checking on (Pi 4, ARMv7: an unaligned
LDR/STR aborts). Some of UnixLib's assembler sources (common/_riscosify.s,
common/_exist.s, signal/_signal.s, sys/_profile.s in GCCSDK's UnixLib, as
built by riscos-crossdev's toolchain) give their .data no alignment (1),
so where the linker puts it depends on the program: in Groove 0.4 to
0.4.2, __riscosify_control_internal landed at an address ending in 6 and
UnixLib's own start-up aborted ("abort on data transfer") unless
alignment exceptions were turned off. Raising sh_addralign to 4 makes the
linker pad before them; nothing else changes.

    tools/align-libs.py IN.a OUT.a      (prints what it changed)
    tools/align-libs.py --check IN.a    (exit 1 if anything needs it)
"""
import struct
import sys

SHT_PROGBITS, SHT_NOBITS = 1, 8
SHF_WRITE, SHF_ALLOC = 1, 2


def fix_object(data, name, check):
    """Return (new bytes, list of changes) for one ELF32 LE object."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        return data, []
    b = bytearray(data)
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x2E)
    def sh(i):
        return struct.unpack_from("<IIIIIIIIII", b, shoff + i * shentsize)
    stroff = sh(shstrndx)[4]
    changes = []
    for i in range(shnum):
        nm, typ, flags, addr, off, size, link, info, align, entsize = sh(i)
        if typ not in (SHT_PROGBITS, SHT_NOBITS):
            continue
        if not (flags & SHF_ALLOC) or not (flags & SHF_WRITE):
            continue                     # (data and bss: read-write)
        if size < 4 or align >= 4:
            continue
        end = b.index(b"\0", stroff + nm)
        sname = b[stroff + nm:end].decode()
        changes.append("%s %s: %d bytes, aligned to %d -> 4" % (name, sname, size, align))
        struct.pack_into("<I", b, shoff + i * shentsize + 32, 4)
    return bytes(b), changes


def main():
    check = sys.argv[1] == "--check"
    src = sys.argv[2] if check else sys.argv[1]
    a = open(src, "rb").read()
    if a[:8] != b"!<arch>\n":
        sys.exit("%s: not an ar archive" % src)
    out = bytearray(a[:8])
    pos, longnames, changes = 8, b"", []
    while pos < len(a):
        hdr = a[pos:pos + 60]
        name = hdr[:16].decode().strip()
        size = int(hdr[48:58].decode().strip())
        body = a[pos + 60:pos + 60 + size]
        if name == "//":
            longnames = body
        elif name.startswith("/") and name[1:].isdigit():
            o = int(name[1:])
            name = longnames[o:longnames.index(b"\n", o)].decode()
        if not name.startswith("/"):
            body, c = fix_object(body, name.rstrip("/"), check)
            changes += c
        out += hdr + body
        pos += 60 + size
        if size % 2:
            out += a[pos:pos + 1]
            pos += 1
    for c in changes:
        print(c)
    if check:
        sys.exit(1 if changes else 0)
    open(sys.argv[2], "wb").write(out)


main()
