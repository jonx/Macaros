; color00.s - the one hosted custom-register spelling: a data-register word
; write to COLOR00, used by classic desktop CPU calibration loops.
; EXPECT: scan SUSPECT -> JIT (the literal remains weak data evidence), runtime
; completes, and MOVE sets N while clearing Z/V/C.
    move.l  #$00008000,d0
    move.w  d0,$dff180
    bmi.s   .flags_ok
    moveq   #1,d0
    rts
.flags_ok:
    moveq   #0,d0
    rts
