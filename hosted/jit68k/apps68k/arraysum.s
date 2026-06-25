; arraysum.s — sum an array of longwords via  move.l (a0)+,d0  in a loop.
; Assembled with vasmm68k_mot -Fhunkexe to a REAL big-endian AmigaOS hunk
; executable WITH a HUNK_DATA section and a HUNK_RELOC32 fixup (the `lea data,a0`
; loads an absolute address that the loader must relocate into the sandbox).
;
;   a0 = &data            ; (lea data,a0 -> a relocated absolute address)
;   d1 = count (5)
;   d0 = 0
;   loop:  add.l (a0)+,d0  ; load longword, post-increment a0       [needs [J5c]]
;          subq.l #1,d1
;          bne    loop
;   return sum in d0       ; 10+20+30+40+50 = 150
;
; DECODER COVERAGE: this combines a MEMORY op with POST-INCREMENT addressing AND a
; loop in one block. [J5a] does register-indirect (An) load/store but NOT
; post-increment; [J5b] does the loop/branch but NO memory. Running BOTH in one
; block (memory + control flow + (An)+) is exactly the cross-cutting coverage
; [J5c] brings. The runner therefore marks this program "needs [J5c]" and proves
; the EXPECTED result (150) + the loader's relocation of `data` via the independent
; reference path — it does NOT claim a JIT pass.

    lea     nums,a0         ; a0 = &nums  (absolute addr -> HUNK_RELOC32 fixup)
    moveq   #5,d1           ; count = 5 elements
    moveq   #0,d0           ; sum = 0
loop:
    add.l   (a0)+,d0        ; sum += *a0++          (post-increment load)
    subq.l  #1,d1           ; count--
    bne.s   loop
    rts                     ; d0 = 150

    section data,data
nums:
    dc.l    10
    dc.l    20
    dc.l    30
    dc.l    40
    dc.l    50
