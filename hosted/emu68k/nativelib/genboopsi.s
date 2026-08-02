; genboopsi.s - a native NewObjectA dispatches OM_NEW into a 68k IClass.

EXEC_OpenLibrary  equ -552
EXEC_CloseLibrary equ -414
DOS_PutStr        equ -948
INT_NewObjectA    equ -636
INT_MakeClass     equ -678
OM_NEW            equ $101
ICLASS_h_Entry    equ 8

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     done
    move.l  d0,a5

    move.l  4.w,a6
    lea     intuitionname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     failed
    move.l  d0,a4

    ; Make a private subclass of rootclass. The returned IClass is a generated
    ; guest-readable facade; install our 68k dispatcher in its Hook prefix.
    move.l  a4,a6
    suba.l  a0,a0
    lea     rootclass(pc),a1
    suba.l  a2,a2
    moveq   #0,d0
    moveq   #0,d1
    jsr     INT_MakeClass(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d6
    move.l  d0,classptr.l
    move.l  d0,a0
    move.l  #dispatcher,ICLASS_h_Entry(a0)
    clr.l   called.l

    ; An empty generated tag domain admits NULL and refuses every tag. Returning
    ; zero from OM_NEW intentionally makes NewObjectA return NULL.
    move.l  d6,a0
    suba.l  a1,a1
    suba.l  a2,a2
    jsr     INT_NewObjectA(a6)
    tst.l   d0
    bne     failed
    cmp.l   #1,called.l
    bne     failed

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

; BOOPSI dispatcher ABI: A0=Class, A2=Object, A1=Msg. For OM_NEW the bridge
; maps the native class sentinel to the same guest facade in both A0 and A2.
dispatcher:
    move.l  classptr.l,d1
    cmp.l   d1,a0
    bne.s   dispatch_bad
    cmp.l   d1,a2
    bne.s   dispatch_bad
    cmp.l   #OM_NEW,(a1)
    bne.s   dispatch_bad
    addq.l  #1,called.l
dispatch_bad:
    moveq   #0,d0
    rts

dosname:       dc.b "dos.library",0
intuitionname: dc.b "intuition.library",0
rootclass:     dc.b "rootclass",0
passmsg:       dc.b "[T3BOOPSI] PASS",10,0
failmsg:       dc.b "[T3BOOPSI] FAIL",10,0
    even
classptr: dc.l 0
called:   dc.l 0
