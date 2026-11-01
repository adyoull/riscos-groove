@ Fake SharedSound fill routines for host/test_handler.py.
@ Data fill: copy words from [R4,R5) to [R1,R2) one for one, until either
@ runs out. Silence fill: write zeros to [R1,R2). Both return R0 = R3 | 1
@ and R9 + 1, and corrupt R10, R11 and R14's neighbours as the real ones may.
@ The last word is the silence routine's offset from the start.
        .arm
        .text
data_fill:
        mov     r10, #0xDE
        mov     r11, #0xAD
1:      cmp     r1, r2
        bhs     2f
        cmp     r4, r5
        bhs     2f
        ldr     r0, [r4], #4
        str     r0, [r1], #4
        b       1b
2:      orr     r0, r3, #1
        add     r9, r9, #1
        mov     pc, lr

silence_fill:
        mov     r10, #0xDE
        mov     r11, #0xAD
        mov     r0, #0
1:      cmp     r1, r2
        strlo   r0, [r1], #4
        blo     1b
        orr     r0, r3, #1
        add     r9, r9, #1
        mov     pc, lr

        .word   silence_fill - data_fill
