; genwindow.s - generated Window facade and GadTools refresh lifecycle.
;
; A custom Screen token crosses inside WA_CustomScreen, OpenWindowTagList
; returns a guest-readable classic Window facade, and GadTools receives the
; original native Window for BeginRefresh/EndRefresh. CloseWindow must release
; every nested facade before the Screen is closed.

EXEC_OpenLibrary   equ -552
EXEC_CloseLibrary  equ -414
DOS_PutStr          equ -948
INT_CloseScreen     equ -66
INT_CloseWindow     equ -72
INT_OpenWindowTags  equ -606
INT_OpenScreenTags  equ -612
GT_BeginRefresh     equ -90
GT_EndRefresh       equ -96

WA_Left          equ $80000064
WA_Top           equ $80000065
WA_Width         equ $80000066
WA_Height        equ $80000067
WA_Title         equ $8000006e
WA_CustomScreen  equ $80000070
WA_DragBar       equ $80000082
WA_DepthGadget   equ $80000083
WA_CloseGadget   equ $80000084
WA_Activate      equ $80000089
WA_SimpleRefresh equ $8000008c
WA_AutoAdjust    equ $80000090

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5

    move.l  4.w,a6
    lea     intuitionname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a4

    move.l  4.w,a6
    lea     gadtoolsname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a3

    move.l  a4,a6
    suba.l  a0,a0
    suba.l  a1,a1
    jsr     INT_OpenScreenTags(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,d5

    lea     windowtags(pc),a1
    move.l  d5,4(a1)              ; WA_CustomScreen typed object
    suba.l  a0,a0
    jsr     INT_OpenWindowTags(a6)
    tst.l   d0
    beq.w   close_screen_failed
    move.l  d0,d6

    move.l  d6,a0
    cmp.w   #260,8(a0)            ; classic Window.Width
    bne.w   close_window_failed
    cmp.w   #100,10(a0)           ; classic Window.Height
    bne.w   close_window_failed
    cmp.l   46(a0),d5             ; Window.WScreen reuses Screen facade
    bne.w   close_window_failed
    tst.l   50(a0)                ; Window.RPort is a typed facade
    beq.w   close_window_failed

    move.l  a3,a6
    move.l  d6,a0
    jsr     GT_BeginRefresh(a6)
    move.l  d6,a0
    moveq   #1,d0
    jsr     GT_EndRefresh(a6)

    move.l  d6,a0
    cmp.w   #260,8(a0)            ; sync preserved the facade
    bne.s   close_window_failed

    move.l  a4,a6
    move.l  d6,a0
    jsr     INT_CloseWindow(a6)
    move.l  d5,a0
    jsr     INT_CloseScreen(a6)
    bra.s   close_libs_pass

close_window_failed:
    move.l  a4,a6
    move.l  d6,a0
    jsr     INT_CloseWindow(a6)
close_screen_failed:
    move.l  d5,a0
    jsr     INT_CloseScreen(a6)
    bra.s   close_libs_fail

close_libs_pass:
    move.l  a3,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    move.l  a4,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    lea     passmsg(pc),a0
    bra.s   say

close_libs_fail:
    move.l  a3,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    move.l  a4,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
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
intuitionname: dc.b "intuition.library",0
gadtoolsname:  dc.b "gadtools.library",0
title:         dc.b "Generated 68k Window",0
passmsg:       dc.b "[T3WINDOW] PASS",10,0
failmsg:       dc.b "[T3WINDOW] FAIL",10,0
    even

windowtags:
    dc.l WA_CustomScreen,0
    dc.l WA_Left,20
    dc.l WA_Top,20
    dc.l WA_Width,260
    dc.l WA_Height,100
    dc.l WA_Title,title
    dc.l WA_DragBar,1
    dc.l WA_DepthGadget,1
    dc.l WA_CloseGadget,1
    dc.l WA_Activate,1
    dc.l WA_SimpleRefresh,1
    dc.l WA_AutoAdjust,1
    dc.l 0,0
