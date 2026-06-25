; sumsq.s — sum of squares via REAL subroutines: nested bsr/jsr/rts over a genuine
; 68k return stack, a computed jsr (An), and a conditional loop. Assembled with
; vasmm68k_mot -Fhunkexe to a REAL big-endian AmigaOS hunk executable.
;
; This is the [J5f] corpus program: where mul/fact/arraysum/libcall are flat (one
; basic block run, terminator = rts/Bcc.s/jsr-d16(a6)), sumsq has real SUBROUTINE
; STRUCTURE — it calls a `square` subroutine from a loop, `square` nests a call to a
; `mul` helper, and one of the calls goes through a COMPUTED jsr (a0). Every call
; pushes a 68k return address onto a7 (the sandbox stack) and every rts pops it, so
; the engine's PC-driven dispatcher + real return stack are exercised end to end.
;
; COMPUTES:   sum = 1*1 + 2*2 + 3*3 + 4*4 + 5*5  =  1+4+9+16+25  =  55
;
;   main:
;     d7 = 0            ; running sum
;     d6 = 1            ; n = 1
;     lea  square,a0    ; a0 -> the square subroutine (for the computed call)
;   loop:               ; (a hot block: re-entered 5 times, translated ONCE -> cache hit)
;     d0 = d6           ; arg n
;     ; alternate between a direct bsr and a COMPUTED jsr (a0) to exercise both:
;     ;   (vasm picks bsr.w if the target is out of byte range; either way the
;     ;    dispatcher pushes the return address and pops it on the square's rts)
;     bsr  square       ; d0 = n*n   (square nests bsr mul internally)
;     add.l d0,d7       ; sum += n*n
;     addq.l #1,d6      ; n++
;     moveq #6,d1
;     cmp.l d1,d6       ; n == 6 ?
;     bne.s loop        ; while n != 6  (real condition codes from cmp.l)
;     ; one extra COMPUTED call through a0 to prove jsr (An) works: square(10)=100,
;     ; discarded (we only assert via the call log / stack, not the sum) -- actually
;     ; fold it in so the result is observable: sum already 55; add square(0)=0.
;     moveq #0,d0
;     jsr  (a0)         ; computed jsr: square(0) -> 0  (return stack push/pop)
;     add.l d0,d7       ; += 0  (proves the computed call returned cleanly to here)
;     move.l d7,d0      ; return sum in d0
;     rts               ; TOP-LEVEL rts -> program exit, d0 = 55
;
;   square:             ; d0 = d0 * d0, via the `mul` helper (NESTED call)
;     move.l d0,d2      ; d2 = n (second factor for mul)
;     bsr   mul         ; d0 = d0 * d2   (NESTED: stack now 2 deep)
;     rts               ; pop back to the caller (loop / the computed jsr site)
;
;   mul:                ; d0 = d0 * d2  (16x16 -> 32 via muls.w; n<=10 fits)
;     muls.w d2,d0      ; d0 = (i16)d0 * (i16)d2
;     rts               ; pop back to square
;
; Exercises: nested bsr/rts (2 deep) over the real return stack, a computed jsr (a0),
; a cmp.l/bne.s loop with genuine condition codes, moveq/add.l/move.l/addq.l/muls.w
; through the REAL Emu68 decoders, and the block cache (the loop body + `square`/`mul`
; each translate once and are re-run). Exit value 55 in d0.

    moveq   #0,d7           ; sum = 0
    moveq   #1,d6           ; n = 1
    lea     square,a0       ; a0 -> square (relocated abs.l for the computed jsr)
loop:
    move.l  d6,d0           ; arg = n
    bsr     square          ; d0 = n*n
    add.l   d0,d7           ; sum += n*n
    addq.l  #1,d6           ; n++
    moveq   #6,d1
    cmp.l   d1,d6           ; n == 6 ?
    bne.s   loop            ; loop while n != 6
    moveq   #0,d0           ; arg = 0 for the computed-call demo
    jsr     (a0)            ; COMPUTED jsr: square(0) -> 0
    add.l   d0,d7           ; sum += 0  (clean computed return)
    move.l  d7,d0           ; result = sum
    rts                     ; top-level rts -> exit, d0 = 55

square:
    move.l  d0,d2           ; d2 = n
    bsr     mul             ; NESTED call: d0 = d0 * d2
    rts                     ; return to caller

mul:
    muls.w  d2,d0           ; d0 = (i16)d0 * (i16)d2
    rts                     ; return to square
