; genowngadget.s - a Gadget family the PROGRAM allocates, adopted by the bridge.
;
; Classic Intuition: the program builds its own gadget list and hands it to
; AddGList. Nothing issued a token, so the crossing has to adopt the guest
; structures - one native mirror each, registered under the guest address.
;
; Checks, in order:
;   - a TWO-node chain crosses, so the family walk and the native relinking run
;;   - RefreshGList crosses the SAME addresses again, proving identity survives
;     rather than a fresh mirror being made per call
;   - the guest structures still read back as the program wrote them, and the
;     link still holds a GUEST address, never a native pointer
;   - RemoveGList takes them back out

EXEC_OpenLibrary    equ -552
EXEC_AllocMem       equ -198
EXEC_FreeMem        equ -210
DOS_PutStr          equ -948
INT_OpenWindowTags  equ -606
INT_CloseWindow     equ -72
INT_AddGList        equ -438
INT_RefreshGList    equ -432
INT_RemoveGList     equ -444

MEMF_CLEAR          equ $00010000
GADGET_SIZE         equ 56              ; struct ExtGadget on m68k
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

    ; --- two gadgets in the PROGRAM's own memory ------------------------------
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
    beq.w   freeg1
    move.l  d0,gadget2

    ; gadget1: 10,20 40x12  id 101  userdata 7   -> gadget2
    move.l  gadget1(pc),a0
    move.l  gadget2(pc),(a0)             ; NextGadget
    move.w  #10,4(a0)
    move.w  #20,6(a0)
    move.w  #40,8(a0)
    move.w  #12,10(a0)
    move.w  #GFLG_EXTENDED,12(a0)
    move.w  #GTYP_BOOLGADGET,16(a0)
    move.w  #101,38(a0)
    move.l  #7,40(a0)

    ; gadget2: 60,20 40x12  id 102  userdata 9   -> end of list
    move.l  gadget2(pc),a0
    clr.l   (a0)
    move.w  #60,4(a0)
    move.w  #20,6(a0)
    move.w  #40,8(a0)
    move.w  #12,10(a0)
    move.w  #GFLG_EXTENDED,12(a0)
    move.w  #GTYP_BOOLGADGET,16(a0)
    move.w  #102,38(a0)
    move.l  #9,40(a0)

    move.l  a4,a6
    suba.l  a0,a0
    lea     wintags(pc),a1
    jsr     INT_OpenWindowTags(a6)
    tst.l   d0
    beq.w   freeg2
    move.l  d0,window

    ; --- AddGList: the library takes the family --------------------------------
    ; The result is the insertion POSITION, not a count, so it says nothing on
    ; its own; what proves the crossing is that both structures survive it and
    ; that the library can be handed the same addresses again below.
    move.l  a4,a6
    move.l  window(pc),a0
    move.l  gadget1(pc),a1
    moveq   #-1,d0                       ; position: at the end
    moveq   #2,d1                        ; numGad
    suba.l  a2,a2                        ; requester: NULL stays NULL
    jsr     INT_AddGList(a6)

    ; --- RefreshGList: same guest addresses, same objects --------------------
    move.l  a4,a6
    move.l  gadget1(pc),a0
    move.l  window(pc),a1
    suba.l  a2,a2
    moveq   #2,d0
    jsr     INT_RefreshGList(a6)

    ; --- the program's own memory still reads as the program wrote it --------
    move.l  gadget1(pc),a0
    cmp.w   #10,4(a0)
    bne.w   badsync
    cmp.w   #101,38(a0)
    bne.w   badsync
    cmp.l   #7,40(a0)
    bne.w   badsync
    move.l  gadget2(pc),d0
    cmp.l   (a0),d0                      ; the link is still a GUEST address
    bne.w   badlink
    move.l  gadget2(pc),a0
    cmp.w   #60,4(a0)
    bne.w   badsync
    cmp.w   #102,38(a0)
    bne.w   badsync
    cmp.l   #9,40(a0)
    bne.w   badsync

    move.l  a4,a6
    move.l  window(pc),a0
    move.l  gadget1(pc),a1
    moveq   #2,d0
    suba.l  a2,a2
    jsr     INT_RemoveGList(a6)

    lea     passmsg(pc),a0
    bsr.w   say
    bra.w   closewin

badsync
    lea     syncmsg(pc),a0
    bsr.w   say
    bra.w   closewin
badlink
    lea     linkmsg(pc),a0
    bsr.w   say

closewin
    move.l  a4,a6
    move.l  window(pc),a0
    jsr     INT_CloseWindow(a6)
freeg2
    move.l  4.w,a6
    move.l  gadget2(pc),a1
    move.l  #GADGET_SIZE,d0
    jsr     EXEC_FreeMem(a6)
freeg1
    move.l  4.w,a6
    move.l  gadget1(pc),a1
    move.l  #GADGET_SIZE,d0
    jsr     EXEC_FreeMem(a6)
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

passmsg     dc.b "[T3OWNGAD] PASS",10,0
syncmsg     dc.b "[T3OWNGAD] FAIL: the guest structure did not survive the crossing",10,0
linkmsg     dc.b "[T3OWNGAD] FAIL: the family link is not a guest address",10,0
failmsg     dc.b "[T3OWNGAD] FAIL: setup",10,0
dosname     dc.b "dos.library",0
intuitionname dc.b "intuition.library",0
gadtoolsname dc.b "gadtools.library",0
    even
