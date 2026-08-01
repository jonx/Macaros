; superviolate.s - privileged instructions: disables interrupts through the SR
; and resets the machine. Supervisor-only, meaningless under translation.
; EXPECT: BANGER -> FULL
    move.w  #$2700,sr
    reset
    rts
