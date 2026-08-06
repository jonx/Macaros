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
INT_ClearMenuStrip  equ -54
INT_DrawImage       equ -114
INT_OpenWindowTags  equ -606
INT_OpenScreenTags  equ -612
INT_SetMenuStrip    equ -264
GT_BeginRefresh     equ -90
GT_EndRefresh       equ -96
GFX_SetFont         equ -66
GFX_InitRastPort    equ -198
GFX_Text            equ -60
GFX_RectFill        equ -306
GFX_SetAPen         equ -342
GFX_ExtendFont      equ -816
GFX_TextFit         equ -696
GFX_NewRegion       equ -516
GFX_DisposeRegion   equ -534
LAY_InstallClipRegion equ -174
LAY_LockLayerInfo   equ -120
LAY_UnlockLayerInfo equ -138
TFE_MATCHWORD       equ $dfe7

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
SA_FullPalette   equ $8000003b
SA_Type          equ $8000002d

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

    move.l  4.w,a6
    lea     graphicsname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,a2

    move.l  4.w,a6
    lea     layersname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,layerbase

    move.l  a4,a6
    lea     newscreen(pc),a0
    lea     screentags(pc),a1
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
    move.l  124(a0),a1            ; Window.WLayer is a retained facade
    beq.w   close_window_failed
    tst.w   142(a1)               ; public Layer.Width was converted
    ble.w   close_window_failed

    ; Screen.LayerInfo is embedded in the public classic Screen, so its
    ; address is screen+224 rather than a separately allocated token.  The
    ; bridge must register that exact guest address as an alias of the native
    ; embedded Layer_Info for direct classic calls such as LockLayerInfo.
    move.l  d5,a0
    lea     224(a0),a0
    move.l  layerbase(pc),a6
    jsr     LAY_LockLayerInfo(a6)
    move.l  d5,a0
    lea     224(a0),a0
    jsr     LAY_UnlockLayerInfo(a6)

    ; A classic program may allocate the retained Menu/MenuItem/IntuiText tree
    ; itself instead of receiving one from GadTools.  Set and clear that tree
    ; to prove nested guest-owned menu mirrors and their lifetime.
    move.l  a4,a6
    move.l  d6,a0
    lea     testmenu(pc),a1
    jsr     INT_SetMenuStrip(a6)
    tst.l   d0
    beq.w   close_window_failed
    move.l  d6,a0
    jsr     INT_ClearMenuStrip(a6)

    ; A nested facade must be complete too.  TurboCalc and other classic GUI
    ; programs read Window.RPort->Layer and pass it straight to layers.library.
    ; Install then remove a clip region to prove that this is a typed Layer
    ; identity, not a copied native pointer or a zero-filled facade field.
    move.l  50(a0),a0
    move.l  (a0),d7               ; RastPort.Layer
    beq.w   close_window_failed
    move.l  a2,a6
    jsr     GFX_NewRegion(a6)
    tst.l   d0
    beq.w   close_window_failed
    move.l  d0,d4
    move.l  layerbase(pc),a6
    move.l  d7,a0
    move.l  d4,a1
    jsr     LAY_InstallClipRegion(a6)
    move.l  d7,a0
    suba.l  a1,a1
    jsr     LAY_InstallClipRegion(a6)
    cmp.l   d4,d0
    bne.w   dispose_region_failed
    move.l  a2,a6
    move.l  d4,a0
    jsr     GFX_DisposeRegion(a6)
    bra.s   region_passed

dispose_region_failed:
    move.l  a2,a6
    move.l  d4,a0
    jsr     GFX_DisposeRegion(a6)
    bra.w   close_window_failed

region_passed:

    ; Program-owned classic Images are retained native mirrors: planar words
    ; are endian-converted, and NextImage is a bounded native chain.
    move.l  d6,a0
    move.l  50(a0),a0
    lea     testimage(pc),a1
    moveq   #2,d0
    moveq   #2,d1
    move.l  a4,a6
    jsr     INT_DrawImage(a6)

    ; A program-owned scratch RastPort is also a first-class mirror.  Populate
    ; its three typed references from the Window's issued RastPort and render
    ; through a generated graphics crossing.
    move.l  a2,a6
    lea     scratchrp(pc),a1
    jsr     GFX_InitRastPort(a6)
    move.l  d6,a0
    move.l  50(a0),a0
    lea     scratchrp(pc),a1
    move.l  (a0),d0
    move.l  d0,(a1)                ; Layer token
    move.l  4(a0),d0
    move.l  d0,4(a1)               ; BitMap token
    move.l  52(a0),d0
    move.l  d0,52(a1)              ; TextFont token
    moveq   #1,d0
    jsr     GFX_SetAPen(a6)
    moveq   #1,d0
    moveq   #1,d1
    moveq   #5,d2
    moveq   #5,d3
    jsr     GFX_RectFill(a6)

    ; Some classic callers only set the low word of a count register.  The
    ; crossing must apply the reviewed 16-bit ABI width both to validation and
    ; to the native call, instead of treating stale high bits as a 4 GiB span.
    lea     narrowtext(pc),a0
    move.l  #$ffff0001,d0
    jsr     GFX_Text(a6)

    ; NULL selects GfxBase->DefaultFont.  The issued RastPort facade must
    ; receive a borrowed guest-readable TextFont token and its text metrics.
    move.l  d6,a0
    move.l  50(a0),a1
    suba.l  a0,a0
    move.l  a2,a6
    jsr     GFX_SetFont(a6)
    move.l  d6,a0
    move.l  50(a0),a0
    tst.l   52(a0)                ; RastPort.Font
    beq.w   close_window_failed
    tst.w   58(a0)                ; RastPort.TxHeight
    beq.w   close_window_failed

    ; ExtendFont installs a public TextFontExtension in tf_Extension (the
    ; Message reply-port slot).  It must be a guest-readable facade, never a
    ; truncated native pointer or NULL after the native call succeeded.
    move.l  52(a0),d4
    move.l  d4,a0
    suba.l  a1,a1
    move.l  a2,a6
    jsr     GFX_ExtendFont(a6)
    tst.l   d0
    beq.w   close_window_failed
    move.l  d4,a0
    move.l  14(a0),a1             ; TextFont.tf_Extension
    move.l  a1,d0
    tst.l   d0
    beq.w   close_window_failed
    cmp.w   #TFE_MATCHWORD,(a1)
    bne.w   close_window_failed
    cmp.l   4(a1),d4              ; extension back-pointer is guest TextFont
    bne.w   close_window_failed

    ; A NULL constraining TextExtent is part of the documented TextFit ABI;
    ; width and height then come from D2/D3.
    move.l  d6,a0
    move.l  50(a0),a1             ; RastPort
    move.l  a3,d7                  ; preserve gadtools before NULL constraint
    move.l  a2,a6                  ; graphics.library before A2 becomes output
    lea     fittext(pc),a0
    moveq   #3,d0
    lea     fitextent(pc),a2
    suba.l  a3,a3                  ; optional constraining extent
    moveq   #1,d1
    move.l  #100,d2
    move.l  #100,d3
    jsr     GFX_TextFit(a6)
    move.l  a6,a2                  ; restore graphics library
    move.l  d7,a3                  ; restore gadtools library
    tst.l   d0
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
    move.l  layerbase(pc),a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    move.l  a2,a1
    jsr     EXEC_CloseLibrary(a6)
    move.l  a3,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    move.l  a4,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    lea     passmsg(pc),a0
    bra.s   say

close_libs_fail:
    move.l  layerbase(pc),a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    move.l  a2,a1
    jsr     EXEC_CloseLibrary(a6)
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
graphicsname:  dc.b "graphics.library",0
layersname:    dc.b "layers.library",0
title:         dc.b "Generated 68k Window",0
passmsg:       dc.b "[T3WINDOW] PASS",10,0
failmsg:       dc.b "[T3WINDOW] FAIL",10,0
fittext:       dc.b "fit",0
narrowtext:    dc.b "x",0
    even

fitextent:     ds.b 12
layerbase:     dc.l 0

testimage:
    dc.w 0,0,16,1,1
    dc.l testimagedata
    dc.b 1,0
    dc.l testimage2
testimage2:
    dc.w 18,0,4,4,0
    dc.l 0
    dc.b 0,2
    dc.l 0
testimagedata:
    dc.w $aaaa
scratchrp:
    ds.b 100

testmenu:
    dc.l 0
    dc.w 0,0,80,10,1
    dc.l testmenuname
    dc.l testmenuitem
    dc.w 0,0,0,0
testmenuitem:
    dc.l 0
    dc.w 0,0,80,10,$0012
    dc.l 0
    dc.l testmenutext
    dc.l 0
    dc.b 0,0
    dc.l 0
    dc.w $ffff
testmenutext:
    dc.b 1,0,0,0
    dc.w 0,0
    dc.l 0,testitemlabel,0
testmenuname:  dc.b "Test",0
testitemlabel: dc.b "Item",0
    even

; A non-NULL classic NewScreen exercises the structure crossing used by
; applications that combine legacy defaults with tag overrides.  The
; TextAttr deliberately has no name: this is valid classic input and asks
; Intuition to fall back to its default screen font.
newscreen:
    dc.w 0,0,320,200,2
    dc.b 1,0
    dc.w 0,$000f
    dc.l newfont,0,0,0

newfont:
    dc.l 0
    dc.w 8
    dc.b 0,0

screentags:
    dc.l SA_FullPalette,$ffffffff
    dc.l SA_Type,$0000000f
    dc.l 0,0

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
