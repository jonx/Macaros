; genowngadgetcycle.s - a guest-owned Gadget family that never ends.
;
; Two gadgets pointing at each other. Walking to the bound and adopting what it
; found would hand the library a truncated list and report success; the walk has
; to say the family exceeded its bound or contains a cycle, and refuse.

EXEC_OpenLibrary    equ -552
EXEC_AllocMem       equ -198
DOS_PutStr          equ -948
INT_OpenWindowTags  equ -606
INT_AddGList        equ -438

MEMF_CLEAR          equ $00010000
GADGET_SIZE         equ 56
GFLG_EXTENDED       equ $8000
GTYP_BOOLGADGET     equ 1

WA_Width            equ $80000066
WA_Height           equ $80000067
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

    move.l  4.w,a6
    move.l  #GADGET_SIZE,d0
    move.l  #MEMF_CLEAR,d1
    jsr     EXEC_AllocMem(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,gadget2

    move.l  gadget1(pc),a0
    move.l  gadget2(pc),(a0)
    move.w  #GFLG_EXTENDED,12(a0)
    move.w  #GTYP_BOOLGADGET,16(a0)
    move.l  gadget2(pc),a0
    move.l  gadget1(pc),(a0)             ; back to the head: a cycle
    move.w  #GFLG_EXTENDED,12(a0)
    move.w  #GTYP_BOOLGADGET,16(a0)

    move.l  a4,a6
    suba.l  a0,a0
    lea     wintags(pc),a1
    jsr     INT_OpenWindowTags(a6)
    tst.l   d0
    beq.w   failed
    move.l  d0,window

    move.l  a4,a6
    move.l  window(pc),a0
    move.l  gadget1(pc),a1
    moveq   #-1,d0
    moveq   #2,d1
    suba.l  a2,a2
    jsr     INT_AddGList(a6)

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
gadget2     dc.l 0

reachedmsg  dc.b "[T3OWNGADCYC] FAIL: a cyclic family crossed",10,0
failmsg     dc.b "[T3OWNGADCYC] FAIL: setup",10,0
dosname     dc.b "dos.library",0
intuitionname dc.b "intuition.library",0
    even
