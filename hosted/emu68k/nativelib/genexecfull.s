; genexecfull.s - one-fixture gate for the complete exec.library batch.
; Every LVO below comes from rom/exec/exec.conf.

EXEC_FindTask             equ -294
EXEC_SetSignal            equ -306
EXEC_SetExcept            equ -312
EXEC_AllocTrap            equ -342
EXEC_FreeTrap             equ -348
EXEC_Cause                equ -180
EXEC_SetSR                equ -144
EXEC_InitSemaphore        equ -558
EXEC_Procure              equ -540
EXEC_Vacate               equ -546
EXEC_RawMayGetChar        equ -510
EXEC_ObtainQuickVector    equ -786
EXEC_NewStackSwap         equ -804
EXEC_TaggedOpenLibrary    equ -810
EXEC_ReadGayle            equ -816
EXEC_AVL_AddNode          equ -852
EXEC_AVL_RemNodeByAddress equ -858
EXEC_AVL_FindNode         equ -870
EXEC_AVL_FindFirstNode    equ -900
EXEC_AVL_FindLastNode     equ -906
EXEC_NewCreateTaskA       equ -918
EXEC_AllocAbs             equ -204
EXEC_CloseLibrary         equ -414
DOS_PutStr                equ -948

TASK_SIGRECVD             equ 26
TASK_EXCEPTDATA           equ 38
TASK_EXCEPTCODE           equ 42
TASKTAG_PC                equ $80100002
TASKTAG_STACKSIZE         equ $80100004
TASKTAG_NAME              equ $80100006

    move.l  4.w,a6

    ; Scalar/host-absent semantics.
    moveq   #0,d0
    moveq   #0,d1
    jsr     EXEC_SetSR(a6)
    cmp.l   #-1,d0
    bne.w   failed
    jsr     EXEC_RawMayGetChar(a6)
    cmp.l   #-1,d0
    bne.w   failed
    jsr     EXEC_ReadGayle(a6)
    tst.l   d0
    bne.w   failed
    move.l  #0,a0
    jsr     EXEC_ObtainQuickVector(a6)
    tst.l   d0
    bne.w   failed

    ; SetSignal returns the prior complete signal word and replaces only the
    ; selected bits.  Ports use the same tc_SigRecvd word, so this also keeps
    ; the normal Exec signal API coherent with Wait/PutMsg.
    move.l  #$00000055,d0
    move.l  #$000000ff,d1
    jsr     EXEC_SetSignal(a6)
    move.l  #$000000a0,d0
    move.l  #$000000f0,d1
    jsr     EXEC_SetSignal(a6)
    and.l   #$000000ff,d0
    cmp.l   #$00000055,d0
    bne.w   failed
    move.l  #0,a0
    jsr     EXEC_FindTask(a6)
    move.l  d0,a4
    move.l  TASK_SIGRECVD(a4),d0
    and.l   #$000000ff,d0
    cmp.l   #$000000a5,d0
    bne.w   failed
    moveq   #0,d0
    move.l  #$000000ff,d1
    jsr     EXEC_SetSignal(a6)

    ; Trap allocation is guest Task bookkeeping, including reuse after free.
    moveq   #-1,d0
    jsr     EXEC_AllocTrap(a6)
    cmp.l   #0,d0
    bne.w   failed
    moveq   #-1,d0
    jsr     EXEC_AllocTrap(a6)
    cmp.l   #1,d0
    bne.w   failed
    moveq   #0,d0
    jsr     EXEC_FreeTrap(a6)
    moveq   #-1,d0
    jsr     EXEC_AllocTrap(a6)
    tst.l   d0
    bne.w   failed

    ; Software interrupt callbacks stay in the guest.
    lea     intmarker(pc),a0
    move.l  a0,intr+14
    lea     intcode(pc),a0
    move.l  a0,intr+18
    lea     intr(pc),a1
    jsr     EXEC_Cause(a6)
    move.l  intmarker(pc),d1
    cmp.l   #$12345678,d1
    bne.w   failed

    ; SetExcept calls the guest exception routine with ExceptData in A1.
    move.l  #0,a1
    jsr     EXEC_FindTask(a6)
    move.l  d0,a4
    lea     exceptmarker(pc),a0
    move.l  a0,TASK_EXCEPTDATA(a4)
    lea     exceptcode(pc),a0
    move.l  a0,TASK_EXCEPTCODE(a4)
    move.l  #1,TASK_SIGRECVD(a4)
    moveq   #1,d0
    moveq   #1,d1
    jsr     EXEC_SetExcept(a6)
    move.l  exceptmarker(pc),d1
    cmp.l   #$5a5a5a5a,d1
    bne.w   failed

    ; The asynchronous semaphore API completes immediately in one context.
    lea     sem(pc),a0
    jsr     EXEC_InitSemaphore(a6)
    lea     sem(pc),a0
    lea     semmsg(pc),a1
    jsr     EXEC_Procure(a6)
    lea     sem(pc),a0
    move.l  semmsg+20(pc),d1
    cmp.l   a0,d1
    bne.w   failed
    lea     semmsg(pc),a1
    jsr     EXEC_Vacate(a6)
    move.l  semmsg+20(pc),d1
    tst.l   d1
    bne.w   failed

    ; NewStackSwap passes eight standard-C stack arguments and returns D0.
    lea     stackbuf(pc),a0
    move.l  a0,sss
    lea     stacktop(pc),a0
    move.l  a0,sss+4
    move.l  a0,sss+8
    lea     sss(pc),a0
    lea     stackcode(pc),a1
    lea     stackargs(pc),a2
    jsr     EXEC_NewStackSwap(a6)
    cmp.l   #42,d0
    bne.w   failed

    ; AVL insertion, callback search, traversal and removal as one family.
    clr.l   avlroot
    move.l  #1,avlnode1+16
    move.l  #2,avlnode2+16
    move.l  #3,avlnode3+16
    lea     avlroot(pc),a0
    lea     avlnode2(pc),a1
    lea     nodecmp(pc),a2
    jsr     EXEC_AVL_AddNode(a6)
    lea     avlroot(pc),a0
    lea     avlnode1(pc),a1
    lea     nodecmp(pc),a2
    jsr     EXEC_AVL_AddNode(a6)
    lea     avlroot(pc),a0
    lea     avlnode3(pc),a1
    lea     nodecmp(pc),a2
    jsr     EXEC_AVL_AddNode(a6)
    move.l  avlroot(pc),a0
    jsr     EXEC_AVL_FindFirstNode(a6)
    lea     avlnode1(pc),a0
    cmp.l   a0,d0
    bne.w   failed
    move.l  avlroot(pc),a0
    jsr     EXEC_AVL_FindLastNode(a6)
    lea     avlnode3(pc),a0
    cmp.l   a0,d0
    bne.w   failed
    move.l  avlroot(pc),a0
    moveq   #2,d1
    move.l  d1,a1
    lea     keycmp(pc),a2
    jsr     EXEC_AVL_FindNode(a6)
    lea     avlnode2(pc),a0
    cmp.l   a0,d0
    bne.w   failed
    lea     avlroot(pc),a0
    lea     avlnode2(pc),a1
    jsr     EXEC_AVL_RemNodeByAddress(a6)
    lea     avlnode2(pc),a0
    cmp.l   a0,d0
    bne.w   failed

    ; NewCreateTaskA creates and runs a second guest Task.
    lea     childcode(pc),a0
    move.l  a0,tasktags+4
    lea     childname(pc),a0
    move.l  a0,tasktags+20
    lea     tasktags(pc),a0
    jsr     EXEC_NewCreateTaskA(a6)
    tst.l   d0
    beq.w   failed
    move.l  childmarker(pc),d1
    cmp.l   #$0badcafe,d1
    bne.w   failed

    ; Fixed-address allocation succeeds without overlapping later allocations.
    moveq   #64,d0
    move.l  #$00800000,a1
    jsr     EXEC_AllocAbs(a6)
    cmp.l   #$00800000,d0
    bne.w   failed

    ; TaggedOpenLibrary(4) is dos.library; use it to print the verdict.
    moveq   #4,d0
    jsr     EXEC_TaggedOpenLibrary(a6)
    tst.l   d0
    beq.w   failed_nodos
    move.l  d0,a5
    lea     passmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
    move.l  4.w,a6
    move.l  a5,a1
    jsr     EXEC_CloseLibrary(a6)
    moveq   #0,d0
    rts

failed:
    move.l  4.w,a6
    moveq   #4,d0
    jsr     EXEC_TaggedOpenLibrary(a6)
    tst.l   d0
    beq.b   failed_nodos
    move.l  d0,a6
    lea     failmsg(pc),a0
    move.l  a0,d1
    jsr     DOS_PutStr(a6)
failed_nodos:
    moveq   #0,d0
    rts

intcode:
    move.l  #$12345678,(a1)
    moveq   #0,d0
    rts
exceptcode:
    move.l  #$5a5a5a5a,(a1)
    moveq   #0,d0
    rts
stackcode:
    move.l  4(sp),d0
    add.l   8(sp),d0
    rts
nodecmp:
    move.l  16(a0),d0
    sub.l   16(a1),d0
    rts
keycmp:
    move.l  16(a0),d0
    sub.l   a1,d0
    rts
childcode:
    move.l  #$0badcafe,childmarker
    rts

    even
intr:         ds.b 22
intmarker:    dc.l 0
exceptmarker: dc.l 0
sem:          ds.b 44
semmsg:       ds.b 24
sss:          ds.b 12
stackargs:    dc.l 19,23,0,0,0,0,0,0
stackbuf:     ds.b 512
stacktop:
avlroot:      dc.l 0
avlnode1:     ds.b 20
avlnode2:     ds.b 20
avlnode3:     ds.b 20
tasktags:
    dc.l TASKTAG_PC,0
    dc.l TASKTAG_STACKSIZE,16384
    dc.l TASKTAG_NAME,0
    dc.l 0,0
childmarker:  dc.l 0
childname:    dc.b "exec-batch-child",0
passmsg:      dc.b "[T3EXECFULL] PASS",10,0
failmsg:      dc.b "[T3EXECFULL] FAIL",10,0
    even
