; ciapeek.s - reads a CIA register ($BFE001, CIA-A port A).
; EXPECT: BANGER -> FULL
    move.b  $bfe001,d0
    and.b   #$40,d0
    rts
