; fact.s — iterative factorial, register-only 68k (assembled with vasmm68k_mot -Fhunkexe).
;
; Computes 5! = 120 by repeated addition (the JIT decoder family covers add.l, not
; mulu), accumulating  fact(n) = product(1..n)  as a sum-of-products loop:
;
;   result = 0
;   term   = 1            ; running "column" we add `multiplier` copies of
;   for k = 2..n:
;       result = term * k  ==  add `term` to itself (k) times
;   ...
;
; To stay inside the register-only opcode set the [J5b]-family decoder handles
; (moveq / add.l Dm,Dn / subq.l #imm,Dn / bne.s / rts — NO mulu, NO memory), we
; compute factorial as NESTED additive loops:
;
;   acc = 1
;   for k = 2..5:                 ; outer: multiply acc by k
;       tmp = 0
;       cnt = k
;     mulloop:                    ; inner: tmp = acc * k  via repeated add
;       tmp = tmp + acc
;       cnt = cnt - 1
;       bne mulloop
;       acc = tmp
;   return acc in d0             ; 1*2*3*4*5 = 120
;
; This is a REALISTIC opcode/addressing-mode mix for the register-only JIT path:
; two nested loops, a backward conditional branch (bne.s) reading real condition
; codes from subq.l, register-direct add.l, and moveq immediates. Exit value 120
; lands in d0 (the AmigaOS process return register), so the runner asserts d0==120.
;
; NOTE on decoder coverage: the OUTER loop uses `move.l Dn,Dm` (acc<-tmp, k count)
; and the inner uses add.l/subq.l/bne.s. `move.l Dn,Dm` (register-to-register) is
; NOT yet in the [J5b] single-block decoder; the runner therefore runs the simpler
; single-loop variant (fib.s / sumloop) NOW and treats the nested form as a
; [J5c]-coverage program (verified via the independent reference interpreter).

    moveq   #1,d0           ; acc = 1            (d0 = the accumulator / result)
    moveq   #2,d2           ; k   = 2            (outer multiplier, runs 2..5)

outer:
    moveq   #0,d1           ; tmp = 0            (inner accumulator)
    move.l  d2,d3           ; cnt = k            (inner trip count)    [needs [J5c]]
inner:
    add.l   d0,d1           ; tmp += acc
    subq.l  #1,d3           ; cnt--              (sets real Z/N/V/C/X)
    bne.s   inner           ; loop while cnt != 0
    move.l  d1,d0           ; acc = tmp          (acc *= k)            [needs [J5c]]
    addq.l  #1,d2           ; k++
    moveq   #6,d4
    cmp.l   d4,d2           ; k == 6 ?           [needs [J5c]: cmp + bne via CC]
    bne.s   outer
    rts                     ; return acc (=120) in d0
