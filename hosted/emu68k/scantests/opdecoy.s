; opdecoy.s - a NEGATIVE control: opcode-shaped constants ($4E70 RESET, $4E73
; RTE, $46C0 MOVE-to-SR) living in a DATA hunk, where they are plainly data.
; EXPECT: CLEAN -> JIT
    lea     decoys,a0
    move.w  (a0),d0
    rts

    section decoys,data
decoys:
    dc.w    $4e70,$4e73,$46c0,$4e72
