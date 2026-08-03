; genframeyield.s - a parent that polls without blocking, and a child that must
; still get to run.
;
; NO window, NO IDCMP, NO native input. This isolates ONE question: does a
; context that never calls a blocking primitive starve its sibling?
;
; The parent sends the child work and then polls its own reply port with GetMsg
; in a loop paced by WaitTOF - which is what a real application's idle loop
; looks like, and which never calls Wait. If the frame wait does not hand the
; turn back, the child never runs, no reply ever arrives, and the parent spins
; until the run is killed. If it does, the reply arrives and this passes.
;
; This certifies the MECHANISM. It is not evidence that any real program has
; this shape - that has to come from real programs, and is counted separately.

EXEC_OpenLibrary  equ -552
EXEC_FindTask     equ -294
EXEC_PutMsg       equ -366
EXEC_GetMsg       equ -372
EXEC_ReplyMsg     equ -378
EXEC_Wait         equ -318
DOS_PutStr        equ -948
DOS_CreateNewProc equ -498
GFX_WaitTOF       equ -270

MP_SIGBIT    equ 15
MP_SIGTASK   equ 16
MP_MSGLIST   equ 20
MN_REPLYPORT equ 14

NP_Entry     equ $800003EB
NP_StackSize equ $800003F3
TAG_DONE     equ 0

; A frame wait is a real wait, so this is seconds of wall clock, not spins of
; nothing. A fair scheduler needs ONE of these to let the child run; this allows
; two hundred before calling it starvation.
SPINS        equ 200

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,dosbase

    move.l  4.w,a6
    lea     gfxname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   nogfx
    move.l  d0,gfxbase

    lea     replyport(pc),a0
    bsr.w   initport
    lea     workport(pc),a0
    bsr.w   initport

    move.l  4.w,a6
    suba.l  a1,a1
    jsr     EXEC_FindTask(a6)
    lea     replyport(pc),a0
    move.l  d0,MP_SIGTASK(a0)
    move.b  #13,MP_SIGBIT(a0)

    move.l  dosbase(pc),a6
    lea     proctags(pc),a0
    move.l  a0,d1
    jsr     DOS_CreateNewProc(a6)
    tst.l   d0
    beq.w   badstart

    ; hand the child its work
    lea     work(pc),a0
    lea     replyport(pc),a1
    move.l  a1,MN_REPLYPORT(a0)
    move.l  #1,20(a0)
    move.l  4.w,a6
    lea     workport(pc),a0
    lea     work(pc),a1
    jsr     EXEC_PutMsg(a6)

    ; ---- the idle loop: GetMsg + WaitTOF, and never Wait -------------------
    move.l  #SPINS,d7
spin
    move.l  4.w,a6
    lea     replyport(pc),a0
    jsr     EXEC_GetMsg(a6)
    tst.l   d0
    bne.s   gotreply
    move.l  gfxbase(pc),a6
    jsr     GFX_WaitTOF(a6)
    subq.l  #1,d7
    bne.s   spin
    lea     starvemsg(pc),a0
    bra.w   say

gotreply
    move.l  d0,a0
    cmp.l   #2,20(a0)                ; the child must have done the work
    bne.w   badwork
    lea     passmsg(pc),a0
    bra.w   say

badstart
    lea     startmsg(pc),a0
    bra.w   say
badwork
    lea     workmsg(pc),a0
    bra.w   say
nogfx
    lea     gfxmsg(pc),a0
say
    move.l  dosbase(pc),a6
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
done
    moveq   #0,d0
    rts

;--------------------------------------------------------------- child
childentry
    move.l  4.w,a6
    suba.l  a1,a1
    jsr     EXEC_FindTask(a6)
    move.l  d0,a3
    lea     workport(pc),a0
    move.l  d0,MP_SIGTASK(a0)
    move.b  #14,MP_SIGBIT(a0)
childloop
    move.l  4.w,a6
    lea     workport(pc),a0
    jsr     EXEC_GetMsg(a6)
    tst.l   d0
    bne.s   childgot
    move.l  4.w,a6
    moveq   #0,d0
    bset    #14,d0
    jsr     EXEC_Wait(a6)            ; the child DOES block, which is the point
    bra.s   childloop
childgot
    move.l  d0,a2
    addq.l  #1,20(a2)
    move.l  4.w,a6
    move.l  a2,a1
    jsr     EXEC_ReplyMsg(a6)
    rts

initport
    lea     MP_MSGLIST(a0),a1
    move.l  a1,d0
    addq.l  #4,d0
    move.l  d0,(a1)
    clr.l   4(a1)
    move.l  a1,8(a1)
    rts

proctags
    dc.l    NP_Entry,childentry
    dc.l    NP_StackSize,16384
    dc.l    TAG_DONE,0

dosbase    dc.l 0
gfxbase    dc.l 0
replyport  ds.b 44
workport   ds.b 44
work       ds.b 32

passmsg   dc.b "[T3YIELD] PASS",10,0
starvemsg dc.b "[T3YIELD] FAIL: the child never ran - a frame wait did not yield",10,0
startmsg  dc.b "[T3YIELD] FAIL: the child process did not start",10,0
workmsg   dc.b "[T3YIELD] FAIL: the reply came back untouched",10,0
gfxmsg    dc.b "[T3YIELD] FAIL: no graphics.library",10,0
dosname   dc.b "dos.library",0
gfxname   dc.b "graphics.library",0
    even
