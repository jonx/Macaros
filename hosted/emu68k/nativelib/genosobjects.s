; genosobjects.s - borrowed handles, guest-owned MsgPort, and native Regions.

EXEC_OpenLibrary       equ -552
EXEC_CloseLibrary      equ -414
EXEC_FindSemaphore     equ -594
EXEC_CreateMsgPort     equ -666
EXEC_DeleteMsgPort     equ -672
DOS_PutStr             equ -948
GFX_OrRectRegion       equ -510
GFX_NewRegion          equ -516
GFX_DisposeRegion      equ -534
GFX_SetRegion          equ -1086
GFX_AreRegionsEqual    equ -1098
GFX_IsPointInRegion    equ -1104
MSGPORT_mp_SigBit      equ 15

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     done
    move.l  d0,a5

    ; Borrowed result: a missing named semaphore is a clean NULL and never
    ; acquires an automatic destructor.
    move.l  4.w,a6
    lea     nosemaphore(pc),a1
    jsr     EXEC_FindSemaphore(a6)
    tst.l   d0
    bne     failed

    ; MsgPort deliberately stays guest-owned: programs embed and walk its
    ; lists. This Exec path runs before the native generator and returns a
    ; genuine guest-layout structure rather than a native facade.
    jsr     EXEC_CreateMsgPort(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d6
    move.l  d0,a0
    moveq   #0,d0
    move.b  MSGPORT_mp_SigBit(a0),d0
    cmp.b   #32,d0
    bhs     failed
    move.l  d6,a0
    jsr     EXEC_DeleteMsgPort(a6)

    move.l  4.w,a6
    lea     gfxname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     failed
    move.l  d0,a4

    ; Opaque Region handles compose across generated calls; Rectangle is a
    ; generated value shadow, rebuilt from the guest's four big-endian WORDs.
    move.l  a4,a6
    jsr     GFX_NewRegion(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d6
    move.l  d6,a0
    lea     rectangle(pc),a1
    jsr     GFX_OrRectRegion(a6)
    tst.l   d0
    beq     failed
    move.l  d6,a0
    moveq   #5,d0
    moveq   #5,d1
    jsr     GFX_IsPointInRegion(a6)
    tst.l   d0
    beq     failed
    move.l  d6,a0
    moveq   #20,d0
    moveq   #20,d1
    jsr     GFX_IsPointInRegion(a6)
    tst.l   d0
    bne     failed

    jsr     GFX_NewRegion(a6)
    tst.l   d0
    beq     failed
    move.l  d0,d7
    move.l  d6,a0
    move.l  d7,a1
    jsr     GFX_SetRegion(a6)
    tst.l   d0
    beq     failed
    move.l  d6,a0
    move.l  d7,a1
    jsr     GFX_AreRegionsEqual(a6)
    tst.l   d0
    beq     failed
    move.l  d7,a0
    jsr     GFX_DisposeRegion(a6)
    move.l  d6,a0
    jsr     GFX_DisposeRegion(a6)

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

dosname:     dc.b "dos.library",0
gfxname:     dc.b "graphics.library",0
nosemaphore: dc.b "emu68k-no-such-semaphore",0
passmsg:     dc.b "[T3OSOBJ] PASS",10,0
failmsg:     dc.b "[T3OSOBJ] FAIL",10,0
    even
rectangle: dc.w 0,0,10,10
