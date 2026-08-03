; genwindowbad.s - a forged Window token must not reach GadTools.

EXEC_OpenLibrary equ -552
DOS_PutStr        equ -948
GT_BeginRefresh   equ -90

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5

    move.l  4.w,a6
    lea     gadtoolsname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,a6

    move.l  #$12345678,a0
    jsr     GT_BeginRefresh(a6)   ; must stop at typed-object lookup

failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:      dc.b "dos.library",0
gadtoolsname: dc.b "gadtools.library",0
failmsg:      dc.b "[T3WINDOW-BAD] FAIL: invalid Window reached native code",10,0
    even
