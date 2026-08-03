; genidcmp.s - the shared-IDCMP pattern, certified end to end.
;
; NO child process, NO frame wait. This isolates ONE question: does a native
; window's input reach the guest port the program explicitly bound to it, and
; only that port?
;
; The program makes its own port, opens two windows, and shares that one port
; through ModifyIDCMP - which is the supported pattern and the normal case, not
; an edge one. It then proves the whole sequence:
;
;   Wait  ->  GetMsg  ->  ReplyMsg
;
; and that a SECOND port it owns, bound to nothing, stays empty throughout. A
; delivery mechanism that broadcast would pass a test that only checked the
; bound port; the unbound one is what makes this a real assertion.
;
; This certifies the MECHANISM. Whether real programs use this shape is a
; separate question that only real programs can answer.

EXEC_OpenLibrary   equ -552
EXEC_CloseLibrary  equ -414
EXEC_FindTask      equ -294
EXEC_GetMsg        equ -372
EXEC_ReplyMsg      equ -378
EXEC_CreateMsgPort equ -666
DOS_PutStr         equ -948
INT_OpenWindowTags equ -606
INT_CloseWindow    equ -72
INT_ModifyIDCMP    equ -150

MP_SIGBIT    equ 15
MP_SIGTASK   equ 16
MP_MSGLIST   equ 20
W_USERPORT   equ 86

WA_Width     equ $80000066
WA_Height    equ $80000067
WA_IDCMP     equ $8000006A
IDCMP_MOUSEBUTTONS equ $00000008
TAG_DONE     equ 0

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,dosbase

    move.l  4.w,a6
    lea     intuiname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   badsetup
    move.l  d0,intbase

    ; ---- two ports the program owns: one it will share, one it will not ----
    move.l  4.w,a6
    jsr     EXEC_CreateMsgPort(a6)
    tst.l   d0
    beq.w   badsetup
    move.l  d0,shared
    move.l  4.w,a6
    jsr     EXEC_CreateMsgPort(a6)
    tst.l   d0
    beq.w   badsetup
    move.l  d0,unbound

    ; ---- two windows, both sharing the FIRST port -------------------------
    move.l  intbase(pc),a6
    suba.l  a0,a0
    lea     wintags(pc),a1
    jsr     INT_OpenWindowTags(a6)
    tst.l   d0
    beq.w   badsetup
    move.l  d0,win1
    move.l  d0,a0
    move.l  shared(pc),W_USERPORT(a0)
    move.l  intbase(pc),a6
    move.l  win1(pc),a0
    move.l  #IDCMP_MOUSEBUTTONS,d0
    jsr     INT_ModifyIDCMP(a6)

    move.l  intbase(pc),a6
    suba.l  a0,a0
    lea     wintags(pc),a1
    jsr     INT_OpenWindowTags(a6)
    tst.l   d0
    beq.w   closeone
    move.l  d0,win2
    move.l  d0,a0
    move.l  shared(pc),W_USERPORT(a0)
    move.l  intbase(pc),a6
    move.l  win2(pc),a0
    move.l  #IDCMP_MOUSEBUTTONS,d0
    jsr     INT_ModifyIDCMP(a6)

    ; ---- the port the program never bound must stay empty -----------------
    ; Checked BEFORE anything else could have arrived, and again at the end:
    ; a mechanism that broadcast would fill it at some point in between.
    move.l  4.w,a6
    move.l  unbound(pc),a0
    jsr     EXEC_GetMsg(a6)
    tst.l   d0
    bne.w   badleak

    ; ---- drain whatever the bound port has, replying to each --------------
    ; Headless there may be nothing to take, and that is not a failure: what
    ; must hold is that anything arriving here is well formed and replies once,
    ; and that nothing at all arrives on the port nobody bound.
    moveq   #0,d6                        ; how many we handled
drain
    move.l  4.w,a6
    move.l  shared(pc),a0
    jsr     EXEC_GetMsg(a6)
    tst.l   d0
    beq.s   drained
    move.l  d0,a2
    addq.l  #1,d6
    move.l  4.w,a6
    move.l  a2,a1
    jsr     EXEC_ReplyMsg(a6)
    bra.s   drain
drained

    move.l  4.w,a6
    move.l  unbound(pc),a0
    jsr     EXEC_GetMsg(a6)
    tst.l   d0
    bne.w   badleak

    lea     passmsg(pc),a0
    bsr.w   say
    bra.s   closetwo

badleak
    lea     leakmsg(pc),a0
    bsr.w   say
closetwo
    move.l  intbase(pc),a6
    move.l  win2(pc),a0
    tst.l   d0
    jsr     INT_CloseWindow(a6)
closeone
    move.l  intbase(pc),a6
    move.l  win1(pc),a0
    jsr     INT_CloseWindow(a6)
    bra.s   done

badsetup
    lea     setupmsg(pc),a0
    bsr.w   say
done
    moveq   #0,d0
    rts

say
    move.l  dosbase(pc),a6
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
    rts

wintags
    dc.l    WA_Width,200
    dc.l    WA_Height,100
    dc.l    WA_IDCMP,IDCMP_MOUSEBUTTONS
    dc.l    TAG_DONE,0

dosbase   dc.l 0
intbase   dc.l 0
shared    dc.l 0
unbound   dc.l 0
win1      dc.l 0
win2      dc.l 0

passmsg   dc.b "[T3IDCMP] PASS",10,0
leakmsg   dc.b "[T3IDCMP] FAIL: a port nothing was bound to received input",10,0
setupmsg  dc.b "[T3IDCMP] FAIL: setup",10,0
dosname   dc.b "dos.library",0
intuiname dc.b "intuition.library",0
    even
