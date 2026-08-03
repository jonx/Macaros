; gendrawbad.s - a bogus RastPort facade must fail before native drawing.

EXEC_OpenLibrary equ -552
DOS_PutStr        equ -948
GT_DrawBevelBoxA  equ -120

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
    moveq   #0,d0
    moveq   #0,d1
    moveq   #1,d2
    moveq   #1,d3
    suba.l  a1,a1
    jsr     GT_DrawBevelBoxA(a6)   ; must stop at typed-object lookup

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
failmsg:      dc.b "[T3DRAW-BAD] FAIL: invalid RastPort reached native code",10,0
    even
