; genbevel.s - the smallest guest reproducer: open a screen, get VisualInfo,
; draw ONE bevel box into the screen's embedded RastPort, tidy up, report.
; No menus. The native control C:BevelProbe runs the identical sequence.

EXEC_OpenLibrary   equ -552
EXEC_CloseLibrary  equ -414
DOS_PutStr         equ -948
INT_CloseScreen    equ -66
INT_OpenScreenTags equ -612
GT_DrawBevelBoxA   equ -120
GT_GetVisualInfoA  equ -126
GT_FreeVisualInfo  equ -132
GT_VisualInfo      equ $80080034
GTBB_Recessed      equ $80080033
GTBB_FrameType     equ $8008004d

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5

    move.l  4.w,a6
    lea     gadtoolsname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a4

    move.l  4.w,a6
    lea     intuitionname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a3

    move.l  a3,a6
    suba.l  a0,a0
    suba.l  a1,a1
    jsr     INT_OpenScreenTags(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,d6

    move.l  a4,a6
    move.l  d6,a0
    suba.l  a1,a1
    jsr     GT_GetVisualInfoA(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,d7

    lea     beveltags(pc),a1
    move.l  d7,4(a1)
    move.l  d6,a0
    lea     84(a0),a0
    moveq   #4,d0
    moveq   #4,d1
    moveq   #32,d2
    moveq   #12,d3
    jsr     GT_DrawBevelBoxA(a6)

    move.l  d7,a0
    jsr     GT_FreeVisualInfo(a6)

    move.l  a3,a6
    move.l  d6,a0
    jsr     INT_CloseScreen(a6)

    lea     passmsg(pc),a0
    bra.s   say
failed:
    lea     failmsg(pc),a0
say:
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:       dc.b "dos.library",0
gadtoolsname:  dc.b "gadtools.library",0
intuitionname: dc.b "intuition.library",0
passmsg:       dc.b "[T3BEVEL] PASS",10,0
failmsg:       dc.b "[T3BEVEL] FAIL",10,0
    cnop 0,4
beveltags:
    dc.l GT_VisualInfo,0
    dc.l GTBB_Recessed,1
    dc.l GTBB_FrameType,1
    dc.l 0,0
