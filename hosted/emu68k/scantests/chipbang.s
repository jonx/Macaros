; chipbang.s - writes a custom-chip register ($DFF180, the background colour).
; The archetypal hardware-banger: no OS call, straight at the metal.
; EXPECT: BANGER -> FULL
    move.w  #$0f0,$dff180
    move.w  #$000,$dff180
    rts
