; gengadgetbad.s - linked-family invalidation must include every member token.
;
; After freeing the context head, reusing the child token must stop at the
; bridge with a stale Gadget diagnostic. Reaching the FAIL print is a bug.

EXEC_OpenLibrary    equ -552
DOS_PutStr           equ -948
INT_OpenScreenTags   equ -612
GT_CreateGadgetA     equ -30
GT_FreeGadgets       equ -36
GT_CreateContext     equ -114
GT_GetVisualInfoA    equ -126
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
    beq.w   failed
    move.l  d0,d6

    clr.l   glist
    lea     glist(pc),a0
    jsr     GT_CreateContext(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,d7

    move.l  d6,newgadget+22
    moveq   #BUTTON_KIND,d0
    move.l  d7,a0
    lea     newgadget(pc),a1
    suba.l  a2,a2
    jsr     GT_CreateGadgetA(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,d4

    move.l  d7,a0
    jsr     GT_FreeGadgets(a6)
    move.l  d4,a0                 ; member token must have been invalidated
    jsr     GT_FreeGadgets(a6)

failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:       dc.b "dos.library",0
intuitionname: dc.b "intuition.library",0
gadtoolsname:  dc.b "gadtools.library",0
label:         dc.b "Family",0
failmsg:       dc.b "[T3GADGET-BAD] FAIL",10,0
    even

glist: dc.l 0

newgadget:
    dc.w 10,10,80,12
    dc.l label
    dc.l 0
    dc.w 78
    dc.l 0
    dc.l 0
    dc.l 0
