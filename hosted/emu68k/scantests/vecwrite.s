; vecwrite.s - installs its own exception handler by writing the vector page
; directly ($68 = level 2 interrupt autovector).
; EXPECT: BANGER -> FULL
    move.l  #handler,$68.l
    rts
handler:
    rte
