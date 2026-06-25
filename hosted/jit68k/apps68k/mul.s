; mul.s — 7 * 6 by repeated addition, register-only single loop. Assembled with
; vasmm68k_mot -Fhunkexe to a REAL big-endian AmigaOS hunk executable.
;
; This program stays ENTIRELY inside the opcode subset the [J5b] single-block
; decoder handles TODAY (moveq / add.l Dm,Dn / subq.l #imm,Dn / bne.s / rts), so
; the runner translates and runs it through the real JIT pipeline NOW and asserts
; the exit value against an independent reference.
;
;   result = 0
;   count  = 7
;   loop:  result += 6        ; add.l d2,d0
;          count  -= 1        ; subq.l #1,d1   (sets real Z/N/V/C/X)
;          bne    loop        ; backward conditional branch reading the subq flags
;   return result in d0       ; 6 added 7 times = 42
;
; Exercises: moveq immediates, register-direct add.l, a flag-setting subq.l, a
; PC-relative bne.s backward branch (the loop), and rts. Exit code 42 in d0.

    moveq   #0,d0           ; result = 0
    moveq   #7,d1           ; count  = 7   (the multiplier / trip count)
    moveq   #6,d2           ; addend = 6   (the multiplicand)
loop:
    add.l   d2,d0           ; result += 6
    subq.l  #1,d1           ; count--      (real condition codes)
    bne.s   loop            ; while count != 0
    rts                     ; d0 = 42
