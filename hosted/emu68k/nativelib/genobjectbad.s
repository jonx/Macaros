; genobjectbad.s - a released object token must never reach native code again.

EXEC_OpenLibrary equ -552
DOS_PutStr       equ -948
LOC_CloseLocale equ -42
LOC_ConvToUpper equ -54
LOC_OpenLocale  equ -156

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5

    move.l  4.w,a6
    lea     localename(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,a6

    suba.l  a0,a0
    jsr     LOC_OpenLocale(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,d6
    move.l  d6,a0
    jsr     LOC_CloseLocale(a6)

    move.l  d6,a0
    moveq   #'a',d0
    jsr     LOC_ConvToUpper(a6)      ; must stop at the stale-token check

failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:    dc.b "dos.library",0
localename: dc.b "locale.library",0
failmsg:    dc.b "[T3OBJ-BAD] FAIL: stale token reached native code",10,0
