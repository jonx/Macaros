; genmenuitem.s - generated Menu facade exposes a typed first MenuItem.
;
; CreateMenusA returns a guest-readable classic Menu facade. Its FirstItem
; field is a typed alias for the native item, accepted by LayoutMenuItemsA.

EXEC_OpenLibrary    equ -552
EXEC_CloseLibrary   equ -414
DOS_PutStr           equ -948
INT_CloseScreen      equ -66
INT_OpenScreenTags   equ -612
GT_CreateMenusA      equ -48
GT_FreeMenus         equ -54
GT_LayoutMenuItemsA  equ -60
GT_GetVisualInfoA    equ -126
GT_FreeVisualInfo    equ -132

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5
    suba.l  a3,a3
    suba.l  a4,a4

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

    move.l  a3,a6
    move.l  d5,a0
    suba.l  a1,a1
    jsr     GT_GetVisualInfoA(a6)
    tst.l   d0
    beq.w   close_screen_failed
    move.l  d0,d6

    lea     newmenus(pc),a0
    suba.l  a1,a1
    jsr     GT_CreateMenusA(a6)
    tst.l   d0
    beq.w   free_visual_failed
    move.l  d0,d7

    move.l  d7,a0
    move.l  18(a0),a0             ; classic Menu.FirstItem typed facade
    move.l  d6,a1
    suba.l  a2,a2
    jsr     GT_LayoutMenuItemsA(a6)
    tst.l   d0
    beq.w   free_menu_failed

    move.l  d7,a0
    jsr     GT_FreeMenus(a6)
    move.l  d6,a0
    jsr     GT_FreeVisualInfo(a6)
    move.l  a4,a6
    move.l  d5,a0
    jsr     INT_CloseScreen(a6)
    bra.s   pass

free_menu_failed:
    move.l  d7,a0
    jsr     GT_FreeMenus(a6)
free_visual_failed:
    move.l  d6,a0
    jsr     GT_FreeVisualInfo(a6)
close_screen_failed:
    move.l  a4,a6
    move.l  d5,a0
    jsr     INT_CloseScreen(a6)
    bra.s   failed

pass:
    lea     passmsg(pc),a0
    bra.s   say
failed:
    lea     failmsg(pc),a0
say:
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)

    move.l  a3,d0
    beq.s   close_intuition
    move.l  a3,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
close_intuition:
    move.l  a4,d0
    beq.s   done
    move.l  a4,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
done:
    moveq   #0,d0
    rts

dosname:       dc.b "dos.library",0
intuitionname: dc.b "intuition.library",0
gadtoolsname:  dc.b "gadtools.library",0
title:         dc.b "Facade",0
item:          dc.b "Menu item",0
passmsg:       dc.b "[T3MENUITEM] PASS",10,0
failmsg:       dc.b "[T3MENUITEM] FAIL",10,0
    even

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
