; Isolate PC-relative DATA addressing: simple vs indexed.
; Exit code: 0 = both correct, 1 = simple broken, 2 = indexed broken.

    ; (1) simple (d16,pc) word load -> must be $1234
    move.w  tbl(pc),d1
    cmp.w   #$1234,d1
    bne     bad1

    ; (2) indexed (d8,pc,Dn.w) word load -> tbl[2] must be $9abc
    moveq   #4,d0
    move.w  tbl(pc,d0.w),d2
    cmp.w   #$9abc,d2
    bne     bad2

    moveq   #0,d0
    rts
bad1:
    moveq   #1,d0
    rts
bad2:
    moveq   #2,d0
    rts

    cnop 0,2
tbl:
    dc.w    $1234
    dc.w    $5678
    dc.w    $9abc
    dc.w    $def0
