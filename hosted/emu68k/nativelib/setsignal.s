; setsignal.s - focused Exec signal-state regression fixture.
;
; It proves that SetSignal updates the same guest tc_SigRecvd word used by
; Wait, Signal and PutMsg: it replaces only masked bits and returns the old
; full signal word.  This is deliberately a raw Exec program, so it runs in
; the standalone host harness as well as under booted AROS.

EXEC_FindTask  equ -294
EXEC_SetSignal equ -306
TASK_SIGRECVD  equ 26

    move.l  4.w,a6
    move.l  #0,a1
    jsr     EXEC_FindTask(a6)
    move.l  d0,a2
    beq.s   failed

    ; Start with known low bits.  Other signal bits, if any, are preserved.
    moveq   #0,d0
    move.l  #$000000ff,d1
    jsr     EXEC_SetSignal(a6)

    move.l  #$00000055,d0
    move.l  #$000000ff,d1
    jsr     EXEC_SetSignal(a6)
    and.l   #$000000ff,d0
    bne.s   failed

    move.l  #$000000a0,d0
    move.l  #$000000f0,d1
    jsr     EXEC_SetSignal(a6)
    and.l   #$000000ff,d0
    cmp.l   #$00000055,d0
    bne.s   failed

    move.l  TASK_SIGRECVD(a2),d0
    and.l   #$000000ff,d0
    cmp.l   #$000000a5,d0
    bne.s   failed

    moveq   #0,d0
    move.l  #$000000ff,d1
    jsr     EXEC_SetSignal(a6)
    moveq   #0,d0
    rts

failed:
    moveq   #1,d0
    rts
