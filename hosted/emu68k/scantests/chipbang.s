; chipbang.s - writes a custom-chip register ($DFF182, colour register 1).
; The archetypal hardware-banger: no OS call, straight at the metal.
; EXPECT: BANGER -> FULL
    move.w  #$0f0,$dff182
    move.w  #$000,$dff182
    rts
