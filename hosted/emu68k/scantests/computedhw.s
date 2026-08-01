; computedhw.s - a NEGATIVE control for the SCAN and a positive for the runtime
; guard: the custom-chip address is COMPUTED at run time from two halves, so it
; appears nowhere in the image and no static scan can find it. Routes JIT, then
; the engine's guard catches the access when it happens.
; EXPECT: scan CLEAN -> JIT, and a runtime hardware event once it runs.
    move.l  #$00df0000,a0
    move.l  #$0000f180,d0
    add.l   d0,a0              ; a0 = $DFF180, never present as a constant
    move.w  #$0f0,(a0)
    rts
