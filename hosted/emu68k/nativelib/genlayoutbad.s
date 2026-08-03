; genlayoutbad.s - negative control for LayoutMenusA image tag policy.
;
; The bridge can rebuild scalar and TextAttr tags, but a guest Image pointer
; needs its own object/facade contract. GTMN_Checkmark must therefore stop at
; the generated tag-domain boundary before native GadTools sees it.

EXEC_OpenLibrary equ -552
DOS_PutStr        equ -948
INT_OpenScreenTags equ -612
GT_CreateMenusA   equ -48
GT_LayoutMenusA   equ -66
GT_GetVisualInfoA equ -126
GTMN_Checkmark    equ $80080041

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
    beq.s   failed
    move.l  d0,a3

    move.l  a3,a6
    suba.l  a0,a0
    suba.l  a1,a1
    jsr     INT_OpenScreenTags(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,d6

    move.l  a4,a6
    move.l  d6,a0
    suba.l  a1,a1
    jsr     GT_GetVisualInfoA(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,d7

    lea     newmenus(pc),a0
    suba.l  a1,a1
    jsr     GT_CreateMenusA(a6)
    tst.l   d0
    beq.s   failed

    move.l  d0,a0
    move.l  d7,a1
    lea     badtags(pc),a2
    jsr     GT_LayoutMenusA(a6)    ; must stop at the tag-domain check

failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:       dc.b "dos.library",0
gadtoolsname:  dc.b "gadtools.library",0
intuitionname: dc.b "intuition.library",0
title:         dc.b "Bridge",0
item:          dc.b "Imported menu",0
failmsg:       dc.b "[T3LAYOUT-BAD] FAIL: Image pointer reached native code",10,0
    even

badtags:
    dc.l GTMN_Checkmark,$12345678
    dc.l 0,0

newmenus:
    dc.b 1,0
    dc.l title,0
    dc.w 0
    dc.l 0,0
    dc.b 2,0
    dc.l item,0
    dc.w 0
    dc.l 0,0
    dc.b 0,0
    dc.l 0,0
    dc.w 0
    dc.l 0,0
