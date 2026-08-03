; genowngadgetbad.s - the guest-owned Gadget cases that must FAIL CLOSED.
;
; A mirror carries exactly the fields the conversion table knows. Two ways a
; program can ask for more, both of which have to be named rather than guessed:
;
;   1. it sets a field the mirror cannot carry - here GadgetRender, a guest
;      pointer to an Image with no native meaning. Quietly dropping it would
;      draw nothing and blame nobody. The check runs on EVERY crossing, not
;      only when the mirror is made, so this is set AFTER a first successful
;      AddGList to prove the second call still refuses it.
;
;   2. it hands over a family that never ends. Truncating at the bound would be
;      a silently short gadget list.
;
; Which case runs is chosen by the argument, so one source covers both without
; either hiding the other.

EXEC_OpenLibrary    equ -552
EXEC_AllocMem       equ -198
DOS_PutStr          equ -948
INT_OpenWindowTags  equ -606
INT_AddGList        equ -438

MEMF_CLEAR          equ $00010000
GADGET_SIZE         equ 56
GFLG_EXTENDED       equ $8000
GTYP_BOOLGADGET     equ 1

WA_Width            equ $80000090
WA_Height           equ $80000091
TAG_DONE            equ 0

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
    move.l  #GADGET_SIZE,d0
    move.l  #MEMF_CLEAR,d1
    jsr     EXEC_AllocMem(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,gadget1

    move.l  gadget1(pc),a0
    clr.l   (a0)
    move.w  #10,4(a0)
    move.w  #20,6(a0)
    move.w  #40,8(a0)
    move.w  #12,10(a0)
    move.w  #GFLG_EXTENDED,12(a0)
    move.w  #GTYP_BOOLGADGET,16(a0)
    move.w  #101,38(a0)

    move.l  a4,a6
    suba.l  a0,a0
    lea     wintags(pc),a1
    jsr     INT_OpenWindowTags(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,window

    ; a first, clean crossing: the mirror now exists
    move.l  a4,a6
    move.l  window(pc),a0
    move.l  gadget1(pc),a1
    moveq   #-1,d0
    moveq   #1,d1
    suba.l  a2,a2
    jsr     INT_AddGList(a6)

    ; NOW set a field the mirror cannot carry. Nothing was rebuilt in between,
    ; so only a per-crossing check can catch this.
    move.l  gadget1(pc),a0
    move.l  #$00042000,18(a0)            ; GadgetRender: a guest pointer

    move.l  a4,a6
    move.l  window(pc),a0
    move.l  gadget1(pc),a1
    moveq   #-1,d0
    moveq   #1,d1
    suba.l  a2,a2
    jsr     INT_AddGList(a6)

    ; unreachable: the crossing above must have failed the run
    lea     reachedmsg(pc),a0
    bsr.w   say
    bra.w   done

failed
    lea     failmsg(pc),a0
    bsr.w   say
done
    moveq   #0,d0
    rts

say
    move.l  a5,a6
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
    rts

wintags
    dc.l    WA_Width,200
    dc.l    WA_Height,100
    dc.l    TAG_DONE,0

window      dc.l 0
gadget1     dc.l 0

reachedmsg  dc.b "[T3OWNGADBAD] FAIL: an uncarryable field crossed",10,0
failmsg     dc.b "[T3OWNGADBAD] FAIL: setup",10,0
dosname     dc.b "dos.library",0
intuitionname dc.b "intuition.library",0
    even
