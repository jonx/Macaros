; genrefused.s - negative control for a reviewed whole-function refusal.
;
; GT_FilterIMsg still cannot cross an arbitrary guest IntuiMessage: its result
; is embedded or allocated mutable state with context-owned lifetime.  The
; generated dispatcher must identify this reviewed refusal before native
; GadTools dereferences the deliberately NULL message.  GT_GetIMsg is no longer
; this negative control: it now has a complete handwritten facade/reply path.

EXEC_OpenLibrary equ -552
DOS_PutStr        equ -948
GT_FilterIMsg     equ -102

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5

    move.l  4.w,a6
    lea     gadtoolsname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   failed

    move.l  d0,a6
    suba.l  a1,a1
    jsr     GT_FilterIMsg(a6)       ; must stop at the reviewed refusal

failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname:      dc.b "dos.library",0
gadtoolsname: dc.b "gadtools.library",0
failmsg:      dc.b "[T3REFUSED] FAIL: refused vector reached native code",10,0
    even
