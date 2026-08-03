; genproc.s - a second 68k process, talked to the way AmigaOS programs do it.
;
; The child is the shape real code uses: find itself, wait on its own port, take
; a message, answer it, repeat, and stop when it is told to. The parent starts
; it, sends it a message, and waits for the reply.
;
; That exchange only completes if all of it works: the child got a context of
; its own, FindTask told it about ITSELF and not about the parent, its Wait
; handed the turn back rather than spinning, the parent's PutMsg signalled it,
; and the parent's own Wait resumed the child and then collected the reply.

EXEC_OpenLibrary equ -552
EXEC_FindTask    equ -294
EXEC_PutMsg      equ -366
EXEC_GetMsg      equ -372
EXEC_ReplyMsg    equ -378
EXEC_Wait        equ -318
DOS_PutStr       equ -948
DOS_CreateNewProc equ -498

MP_SIGBIT    equ 15
MP_SIGTASK   equ 16
MP_MSGLIST   equ 20
MN_REPLYPORT equ 14
MN_LENGTH    equ 18
TC_SIGRECVD  equ 26

NP_Entry     equ $800003EB
NP_StackSize equ $800003F3
TAG_DONE     equ 0

;--------------------------------------------------------------- parent
    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5
    move.l  d0,dosbase

    lea     parentport(pc),a0
    bsr.w   initport
    lea     childport(pc),a0
    bsr.w   initport

    ; our own port, on signal bit 13
    move.l  4.w,a6
    suba.l  a1,a1
    jsr     EXEC_FindTask(a6)
    move.l  d0,parenttask
    lea     parentport(pc),a0
    move.l  d0,MP_SIGTASK(a0)
    move.b  #13,MP_SIGBIT(a0)

    ; start the child
    move.l  a5,a6
    lea     proctags(pc),a0
    move.l  a0,d1
    jsr     DOS_CreateNewProc(a6)
    tst.l   d0
    beq.w   badstart

    ; send it a message that names our port as the reply port
    lea     work(pc),a0
    lea     parentport(pc),a1
    move.l  a1,MN_REPLYPORT(a0)
    move.w  #20,MN_LENGTH(a0)
    move.l  #$5A5A0000,20(a0)            ; the payload the child transforms

    move.l  4.w,a6
    lea     childport(pc),a0
    lea     work(pc),a1
    jsr     EXEC_PutMsg(a6)

    ; wait for it to come back
    move.l  4.w,a6
    moveq   #0,d0
    bset    #13,d0
    jsr     EXEC_Wait(a6)

    move.l  4.w,a6
    lea     parentport(pc),a0
    jsr     EXEC_GetMsg(a6)
    tst.l   d0
    beq.w   badreply
    move.l  d0,a0
    cmp.l   #$5A5A0001,20(a0)            ; the child must have touched it
    bne.w   badwork

    lea     passmsg(pc),a0
    bra.w   say
badstart
    lea     startmsg(pc),a0
    bra.w   say
badreply
    lea     replymsg(pc),a0
    bra.w   say
badwork
    lea     workmsg(pc),a0
say
    move.l  dosbase(pc),a6
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
done
    moveq   #0,d0
    rts

;--------------------------------------------------------------- child
; find itself, adopt the port, then wait / take / answer.
childentry
    move.l  4.w,a6
    suba.l  a1,a1
    jsr     EXEC_FindTask(a6)
    move.l  d0,a3                        ; OUR task, not the parent's
    lea     childport(pc),a0
    move.l  d0,MP_SIGTASK(a0)
    move.b  #14,MP_SIGBIT(a0)

childloop
    move.l  4.w,a6
    lea     childport(pc),a0
    jsr     EXEC_GetMsg(a6)
    tst.l   d0
    bne.s   childgot

    move.l  4.w,a6
    moveq   #0,d0
    bset    #14,d0
    jsr     EXEC_Wait(a6)                ; hands the turn back to the parent
    bra.s   childloop

childgot
    move.l  d0,a2
    addq.l  #1,20(a2)                    ; the transformation the parent checks
    move.l  4.w,a6
    move.l  a2,a1
    jsr     EXEC_ReplyMsg(a6)
    rts                                  ; one message is enough: exit

; NewList(a0): an empty exec list points its head at its own tail
initport
    lea     MP_MSGLIST(a0),a1
    move.l  a1,d0
    addq.l  #4,d0
    move.l  d0,(a1)
    clr.l   4(a1)
    move.l  a1,8(a1)
    rts

proctags
    dc.l    NP_Entry
    dc.l    childentry
    dc.l    NP_StackSize,16384
    dc.l    TAG_DONE,0

dosbase     dc.l 0
parenttask  dc.l 0
parentport  ds.b 40
childport   ds.b 40
work        ds.b 32

passmsg   dc.b "[T3PROC] PASS",10,0
startmsg  dc.b "[T3PROC] FAIL: the 68k process did not start",10,0
replymsg  dc.b "[T3PROC] FAIL: no reply came back from the child",10,0
workmsg   dc.b "[T3PROC] FAIL: the child did not do the work",10,0
dosname   dc.b "dos.library",0
    even
