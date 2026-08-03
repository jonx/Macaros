; gengadget.s - generated linked Gadget family and NewGadget crossing.
;
; CreateContext writes and returns the same typed head token. CreateGadgetA
; rebuilds a classic 30-byte NewGadget (including nested TextAttr/string and
; VisualInfo object fields), appends a native Gadget, and returns a borrowed
; member token. FreeGadgets destroys the family through its head.

EXEC_OpenLibrary    equ -552
EXEC_CloseLibrary   equ -414
DOS_PutStr           equ -948
INT_CloseScreen      equ -66
INT_OpenScreenTags   equ -612
GT_CreateGadgetA     equ -30
GT_FreeGadgets       equ -36
GT_CreateContext     equ -114
GT_GetVisualInfoA    equ -126
GT_FreeVisualInfo    equ -132
BUTTON_KIND          equ 1

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

    move.l  a3,a6
    move.l  d5,a0
    suba.l  a1,a1
    jsr     GT_GetVisualInfoA(a6)
    tst.l   d0
    beq.w   close_screen_failed
    move.l  d0,d6

    clr.l   glist
    lea     glist(pc),a0
    jsr     GT_CreateContext(a6)
    tst.l   d0
    beq.w   free_visual_failed
    cmp.l   glist(pc),d0
    bne.w   free_context_failed
    move.l  d0,d7

    move.l  d6,newgadget+22
    moveq   #BUTTON_KIND,d0
    move.l  d7,a0
    lea     newgadget(pc),a1
    suba.l  a2,a2
    jsr     GT_CreateGadgetA(a6)
    tst.l   d0
    beq.w   free_context_failed

    move.l  d7,a0
    jsr     GT_FreeGadgets(a6)
    move.l  d6,a0
    jsr     GT_FreeVisualInfo(a6)

    move.l  a4,a6
    move.l  d5,a0
    jsr     INT_CloseScreen(a6)
    bra.s   close_libs_pass

free_context_failed:
    move.l  d7,a0
    jsr     GT_FreeGadgets(a6)
free_visual_failed:
    move.l  d6,a0
    jsr     GT_FreeVisualInfo(a6)
close_screen_failed:
    move.l  a4,a6
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
fontname:      dc.b "topaz.font",0
label:         dc.b "Generated Gadget",0
passmsg:       dc.b "[T3GADGET] PASS",10,0
failmsg:       dc.b "[T3GADGET] FAIL",10,0
    even

glist: dc.l 0

; Classic TextAttr layout: pointer, UWORD size, UBYTE style, UBYTE flags.
textattr:
    dc.l fontname
    dc.w 8
    dc.b 0,0

; Classic packed-to-two NewGadget layout: exactly 30 bytes.
newgadget:
    dc.w 20,20,150,14             ; left, top, width, height
    dc.l label                    ; ng_GadgetText
    dc.l textattr                 ; ng_TextAttr
    dc.w 77                       ; ng_GadgetID
    dc.l 0                        ; ng_Flags
    dc.l 0                        ; ng_VisualInfo, filled at runtime
    dc.l $12345678                ; opaque guest ng_UserData cookie
