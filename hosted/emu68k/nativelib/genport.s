; genport.s - a message port the program owns, used the way exec uses one.
;
; The port, the message, and the list that links them are all the PROGRAM's
; memory. So these are guest-memory list operations, and the test is whether a
; message put on a port comes back off it as the SAME message, with the list
; left in the state exec leaves it in - and whether ReplyMsg routes it to the
; reply port the message names rather than dropping it.
;
;   1. PutMsg then GetMsg returns the message, and the port is then empty
;   2. PutMsg sets the signal bit the port names, which is what makes a real
;      WaitPort return
;   3. ReplyMsg sends the message to its reply port, not into a hole
;   4. two messages come back in the order they were put on

EXEC_OpenLibrary equ -552
EXEC_PutMsg      equ -366
EXEC_GetMsg      equ -372
EXEC_ReplyMsg    equ -378
EXEC_FindTask    equ -294
DOS_PutStr       equ -948

MP_SIGBIT    equ 15
MP_SIGTASK   equ 16
MP_MSGLIST   equ 20
MN_REPLYPORT equ 14
TC_SIGRECVD  equ 26

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5

    ; ---- NewList on both ports, by hand: lh_Head = &lh_Tail, lh_TailPred = &lh_Head
    lea     port(pc),a0
    bsr.w   initport
    lea     reply(pc),a0
    bsr.w   initport

    ; the port belongs to this task, and names signal bit 12
    move.l  4.w,a6
    suba.l  a1,a1
    jsr     EXEC_FindTask(a6)
    move.l  d0,a3                    ; a3 = our Task
    lea     port(pc),a0
    move.l  d0,MP_SIGTASK(a0)
    move.b  #12,MP_SIGBIT(a0)

    ; clear the signal we are about to watch for
    move.l  a3,a0
    moveq   #0,d0
    move.l  d0,TC_SIGRECVD(a0)

    ; ---- 1+2: put a message on, and the bit must be set -------------------
    lea     msg1(pc),a0
    lea     reply(pc),a1
    move.l  a1,MN_REPLYPORT(a0)
    move.l  4.w,a6
    lea     port(pc),a0
    lea     msg1(pc),a1
    jsr     EXEC_PutMsg(a6)

    move.l  a3,a0
    move.l  TC_SIGRECVD(a0),d0
    btst    #12,d0
    beq.w   badsig

    ; ---- and a second one, to check the order -----------------------------
    move.l  4.w,a6
    lea     port(pc),a0
    lea     msg2(pc),a1
    jsr     EXEC_PutMsg(a6)

    ; ---- GetMsg gives them back in order ----------------------------------
    move.l  4.w,a6
    lea     port(pc),a0
    jsr     EXEC_GetMsg(a6)
    lea     msg1(pc),a0
    cmp.l   a0,d0
    bne.w   badorder
    move.l  d0,a4                    ; keep msg1 for the reply test

    move.l  4.w,a6
    lea     port(pc),a0
    jsr     EXEC_GetMsg(a6)
    lea     msg2(pc),a0
    cmp.l   a0,d0
    bne.w   badorder

    ; the port must now be empty
    move.l  4.w,a6
    lea     port(pc),a0
    jsr     EXEC_GetMsg(a6)
    tst.l   d0
    bne.w   badempty

    ; ---- 3: ReplyMsg routes to the reply port -----------------------------
    move.l  4.w,a6
    move.l  a4,a1
    jsr     EXEC_ReplyMsg(a6)

    move.l  4.w,a6
    lea     reply(pc),a0
    jsr     EXEC_GetMsg(a6)
    cmp.l   a4,d0
    bne.w   badreply

    lea     passmsg(pc),a0
    bra.w   say
badsig
    lea     sigmsg(pc),a0
    bra.w   say
badorder
    lea     ordermsg(pc),a0
    bra.w   say
badempty
    lea     emptymsg(pc),a0
    bra.w   say
badreply
    lea     replymsg(pc),a0
say
    move.l  a5,a6
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
done
    moveq   #0,d0
    rts

; NewList(a0): an empty exec list points its head at its own tail
initport
    lea     MP_MSGLIST(a0),a1
    move.l  a1,d0
    addq.l  #4,d0
    move.l  d0,(a1)                  ; lh_Head = &lh_Tail
    clr.l   4(a1)                    ; lh_Tail = NULL
    move.l  a1,8(a1)                 ; lh_TailPred = &lh_Head
    rts

port    ds.b 40
reply   ds.b 40
msg1    ds.b 24
msg2    ds.b 24

passmsg  dc.b "[T3PORT] PASS",10,0
sigmsg   dc.b "[T3PORT] FAIL: PutMsg did not signal the port's task",10,0
ordermsg dc.b "[T3PORT] FAIL: messages did not come back in order",10,0
emptymsg dc.b "[T3PORT] FAIL: the port was not empty after both were taken",10,0
replymsg dc.b "[T3PORT] FAIL: ReplyMsg did not reach the reply port",10,0
dosname  dc.b "dos.library",0
    even
