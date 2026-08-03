; genrecord.s - production proof for generated terminated-record arrays.
;
; A classic 20-byte NewMenu array is rebuilt as native 40-byte records,
; including its guest string pointers and opaque user-data cookie. The native
; Menu result crosses back as a typed token and FreeMenus consumes it.

EXEC_OpenLibrary  equ -552
EXEC_CloseLibrary equ -414
DOS_PutStr         equ -948
INT_CloseScreen     equ -66
INT_OpenScreenTags  equ -612
GT_CreateMenusA    equ -48
GT_FreeMenus       equ -54
GT_LayoutMenusA    equ -66
GT_GetVisualInfoA  equ -126
GT_FreeVisualInfo  equ -132
GT_DrawBevelBoxA   equ -120
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

    move.l  a4,a6
    lea     newmenus(pc),a0
    suba.l  a1,a1
    jsr     GT_CreateMenusA(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,d5

    move.l  d5,a0
    move.l  d7,a1
    suba.l  a2,a2
    jsr     GT_LayoutMenusA(a6)
    tst.l   d0
    beq.s   failed

    lea     beveltags(pc),a1
    move.l  d7,4(a1)
    move.l  d6,a0
    lea     84(a0),a0              ; embedded classic Screen.RastPort facade
    moveq   #4,d0
    moveq   #4,d1
    moveq   #32,d2
    moveq   #12,d3
    jsr     GT_DrawBevelBoxA(a6)

    move.l  d5,a0
    jsr     GT_FreeMenus(a6)
    move.l  d7,a0
    jsr     GT_FreeVisualInfo(a6)

    move.l  a3,a6
    move.l  d6,a0
    jsr     INT_CloseScreen(a6)

    move.l  a3,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)

    move.l  a4,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)

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

dosname:      dc.b "dos.library",0
gadtoolsname: dc.b "gadtools.library",0
intuitionname: dc.b "intuition.library",0
title:        dc.b "Bridge",0
item:         dc.b "Imported menu",0
key:          dc.b "I",0
passmsg:      dc.b "[T3RECORD] PASS",10,0
failmsg:      dc.b "[T3RECORD] FAIL",10,0
    even

beveltags:
    dc.l GT_VisualInfo,0
    dc.l GTBB_Recessed,1
    dc.l GTBB_FrameType,1
    dc.l 0,0

; Classic packed-to-two NewMenu layout: 20 bytes per record.
newmenus:
    dc.b 1,0                       ; NM_TITLE, alignment pad
    dc.l title,0                   ; nm_Label, nm_CommKey
    dc.w 0                         ; nm_Flags
    dc.l 0,$12345678               ; nm_MutualExclude, nm_UserData

    dc.b 2,0                       ; NM_ITEM, alignment pad
    dc.l item,key                  ; nm_Label, nm_CommKey
    dc.w 0
    dc.l 0,$89abcdef

    dc.b 0,0                       ; NM_END sentinel is itself a record
    dc.l 0,0
    dc.w 0
    dc.l 0,0
