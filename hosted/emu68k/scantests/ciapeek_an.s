; ciapeek_an.s - the same CIA read as ciapeek.s but through an address register,
; to check the (An) path guards identically to the absolute path.
    move.l  #$00bfe001,a0
    move.b  (a0),d0
    rts
