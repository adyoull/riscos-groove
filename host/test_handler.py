#!/usr/bin/env python3
"""Run riscos/handler.s (the SharedSound handler) in Unicorn, as ARM code.

The handler is assembled, copied to an odd address (as sound.c copies it
into the RMA, so it must be position independent), and called the way
SharedSound calls it, with fake fill routines that copy words 1:1 (no
resampling), write zeros, and corrupt every register SharedSound's real
routines are allowed to corrupt. Each case checks the output buffer, the new
play offset, the underrun count, R3 bit 0 and the registers the handler must
preserve.

usage: test_handler.py HANDLER_BIN FILLS_BIN     (made by the Makefile)
"""
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import *

CODE = 0x23450          # where the handler is "copied" (not page aligned on purpose)
FILLS = 0x30000         # fake fill routines: +0 data fill, +N silence fill
TABLE = 0x40000         # SharedSound's table of fill routines
RING = 0x50000          # the ring block
BUF = 0x60000           # the ring buffer
OUT = 0x70000           # SharedSound's output buffer
STACK = 0x90000
RETURN = 0xA0000        # the handler returns here
RING_WORDS = 16

failures = 0
count = 0


def check(cond, what):
    global failures, count
    count += 1
    if not cond:
        failures += 1
        print("FAIL", what)


def run(handler, fills, play, fill, out_words, mix=False):
    """Ring holds words 1000+i at index i. Returns (output, play, underruns, regs)."""
    mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    mu.mem_map(0x20000, 0x90000)
    mu.mem_write(CODE, handler)
    mu.mem_write(FILLS, fills)
    data_fill = FILLS
    silence_fill = FILLS + fills_silence_offset
    mu.mem_write(TABLE, struct.pack("<III", 0, silence_fill, data_fill))
    mu.mem_write(BUF, struct.pack("<%dI" % RING_WORDS, *[1000 + i for i in range(RING_WORDS)]))
    mu.mem_write(RING, struct.pack("<6I", BUF, BUF + RING_WORDS * 4, play * 4, fill * 4, 7, 0))
    mu.mem_write(OUT, struct.pack("<%dI" % out_words, *([0x5555] * out_words)))
    regs = {UC_ARM_REG_R0: RING, UC_ARM_REG_R1: OUT, UC_ARM_REG_R2: OUT + out_words * 4,
            UC_ARM_REG_R3: 1 if mix else 0, UC_ARM_REG_R4: 44100 << 10, UC_ARM_REG_R5: 0x55,
            UC_ARM_REG_R6: 1 << 24, UC_ARM_REG_R7: 0xFFFFFFFF, UC_ARM_REG_R8: TABLE,
            UC_ARM_REG_R9: 0x99, UC_ARM_REG_R10: 0xAAAA, UC_ARM_REG_R11: 0xBBBB,
            UC_ARM_REG_R12: 0xCCCC, UC_ARM_REG_SP: STACK, UC_ARM_REG_LR: RETURN}
    for r, v in regs.items():
        mu.reg_write(r, v)
    mu.emu_start(CODE, RETURN, count=10000)
    out = list(struct.unpack("<%dI" % out_words, mu.mem_read(OUT, out_words * 4)))
    block = struct.unpack("<6I", mu.mem_read(RING, 24))
    after = {r: mu.reg_read(r) for r in regs}
    return out, block[2] // 4, block[5], block[4], after, regs


def case(handler, fills, name, play, fill, n, want_out, want_play, want_underruns):
    out, newplay, under, acc, after, before = run(handler, fills, play, fill, n)
    check(out == want_out, "%s: output %s, want %s" % (name, out, want_out))
    check(newplay == want_play, "%s: play offset %d, want %d" % (name, newplay, want_play))
    check(under == want_underruns, "%s: underruns %d, want %d" % (name, under, want_underruns))
    check(after[UC_ARM_REG_R3] & 1, "%s: R3 bit 0 set on exit" % name)
    check(acc > 7, "%s: the accumulator from the fill routine is saved" % name)
    for r in (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
              UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP):
        check(after[r] == before[r], "%s: register %d preserved" % (name, r))


def main():
    global fills_silence_offset
    handler = open(sys.argv[1], "rb").read()
    fills = open(sys.argv[2], "rb").read()
    fills_silence_offset = struct.unpack("<I", fills[-4:])[0]
    w = lambda a, b: [1000 + i for i in range(a, b)]
    case(handler, fills, "empty ring (idle: not an underrun)", 3, 3, 4, [0] * 4, 3, 0)
    case(handler, fills, "enough data", 2, 10, 4, w(2, 6), 6, 0)
    case(handler, fills, "short of data", 2, 4, 4, w(2, 4) + [0, 0], 4, 1)
    case(handler, fills, "wrapped, enough before the end", 10, 4, 4, w(10, 14), 14, 0)
    case(handler, fills, "wrapped, crosses the end", 14, 4, 4, w(14, 16) + w(0, 2), 2, 0)
    case(handler, fills, "wrapped, ends exactly at the end", 12, 4, 4, w(12, 16), 0, 0)
    case(handler, fills, "wrapped, runs dry", 14, 1, 4, w(14, 16) + w(0, 1) + [0], 1, 1)
    case(handler, fills, "wrapped, nothing at the start", 14, 0, 4, w(14, 16) + [0, 0], 0, 1)
    print("%d checks, %d failed" % (count, failures))
    sys.exit(1 if failures else 0)


main()
