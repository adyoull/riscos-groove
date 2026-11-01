@ Groove - the SharedSound handler.
@
@ SharedSound calls this at interrupt time to fill (or mix into) the sound
@ buffer. Another task's memory may be paged in at that moment (riscos-crossdev
@ RISCOS-RULES.md, rule 5), so this code and the ring buffer it reads must be
@ in the RMA: sound.c copies the bytes between groove_handler and
@ groove_handler_end into an RMA block, so the code must be position
@ independent - no literal pools, no absolute addresses, only registers and
@ the ring block passed as the handler's parameter.
@
@ Entry (PRM SharedSound_InstallHandler):
@   R0 = parameter = the ring block (below)
@   R1 = start of the buffer to fill, R2 = its end
@   R3 = flags (bit 0 set: mix into the buffer; clear: overwrite it)
@   R4 = sample frequency, R5 = sample period
@   R6 = fractional step (set by SharedSound_SampleRate for this handler)
@   R7 = LR volume
@   R8 = table: [R8,#4] silence fill routine, [R8,#8] data fill routine
@ Exit:
@   R3 = flags; bit 0 must be set: data has been written (SharedSound can't
@        cope with none, so we always write something, at worst silence)
@
@ SharedSound's own fill routines do the mixing, the volume and the
@ resampling from our 44.1 kHz to the output rate. They take
@   R1 dest, R2 dest end, R3 flags, R4 source, R5 source end,
@   R6 step, R7 volume, R9 fractional accumulator
@ and return R0 new flags, R1 next dest, R4 next source, R9 accumulator,
@ preserving R2, R3, R5-R8 and R12 and corrupting the rest (so the ring
@ block pointer lives in R12).
@
@ The ring block (all byte values; the ring holds 32-bit words, left in the
@ top half and right in the bottom half):
@   +0  buffer start          (address)
@   +4  buffer end            (address, exclusive)
@   +8  play offset           written only here
@   +12 fill offset           written only by the task (sound.c)
@   +16 fractional accumulator
@   +20 underrun count        times we ran dry partway through a buffer,
@                             for the status display

        .arm
        .text
        .global groove_handler
        .global groove_handler_end

groove_handler:
        stmfd   sp!, {r0-r2, r4-r12, lr}
        mov     r12, r0
        ldr     r9, [r12, #16]          @ accumulator
        ldr     r10, [r12, #0]          @ buffer start
        ldr     r4, [r12, #8]
        add     r4, r4, r10             @ R4 = play pointer
        ldr     r5, [r12, #12]
        add     r5, r5, r10             @ R5 = fill pointer
        cmp     r4, r5
        beq     idle                    @ empty: nothing queued, not an underrun
        blo     contiguous              @ play < fill: one run [play, fill)

        @ Wrapped: first [play, buffer end), then [buffer start, fill).
        ldr     r5, [r12, #4]
        mov     lr, pc
        ldr     pc, [r8, #8]            @ data fill
        cmp     r4, r5                  @ used it all up to the end?
        ldrhs   r4, [r12, #0]           @ then carry on from the start
        cmp     r1, r2
        bhs     finish                  @ output full
        ldr     r10, [r12, #0]
        ldr     r5, [r12, #12]
        add     r5, r5, r10             @ R5 = fill pointer again
        cmp     r4, r5
        bhs     silence                 @ nothing at the start either

contiguous:
        mov     lr, pc
        ldr     pc, [r8, #8]            @ data fill
        cmp     r1, r2
        bhs     finish

silence:
        @ Ran dry partway through the output: an underrun (heard as a
        @ click). Count it, then fill the rest with silence.
        ldr     r10, [r12, #20]
        add     r10, r10, #1
        str     r10, [r12, #20]
idle:
        @ Silence (mixing silence leaves other handlers' sound alone). An
        @ empty ring at the start isn't counted: Groove lets it drain when
        @ nothing is playing, and Play can come just before the first fill.
        mov     lr, pc
        ldr     pc, [r8, #4]            @ silence fill

finish:
        ldr     r10, [r12, #0]
        sub     r4, r4, r10             @ new play offset
        ldr     r11, [r12, #4]
        sub     r11, r11, r10           @ ring size
        cmp     r4, r11
        movhs   r4, #0                  @ exactly at the end: wrap
        str     r4, [r12, #8]
        str     r9, [r12, #16]
        orr     r3, r3, #1              @ we always wrote data
        ldmfd   sp!, {r0-r2, r4-r12, pc}

groove_handler_end:
