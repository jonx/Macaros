; datadecoy.s - a NEGATIVE control: hardware-shaped constants sitting inline in
; the code hunk as data (a lookup table), never used as an address. A linear
; scan sees the bytes; the confidence grading must call them weak.
; EXPECT: SUSPECT (weak evidence only) -> JIT
    moveq   #0,d0
    lea     table(pc),a0
    move.l  (a0),d1
    rts
table:
    dc.l    $00dff180
    dc.l    $00bfe001
    dc.l    $00dff09a
