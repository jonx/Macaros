; genhookbad.s - an invalid guest Hook entry must fail before native callback.

EXEC_OpenLibrary equ -552
DOS_PutStr       equ -948
UTIL_CallHookPkt equ -102

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5
    move.l  4.w,a6
    lea     utilname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,a6
    lea     badhook(pc),a0
    suba.l a2,a2
    suba.l a1,a1
    jsr     UTIL_CallHookPkt(a6)
failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:  dc.b "dos.library",0
utilname: dc.b "utility.library",0
failmsg:  dc.b "[T3HOOK-BAD] FAIL: invalid entry reached native code",10,0
    even
badhook: dc.l 0,0,0,0,0
