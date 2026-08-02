; genosobjectsbad.s - a disposed native Region must be rejected as stale.

EXEC_OpenLibrary equ -552
DOS_PutStr       equ -948
GFX_NewRegion    equ -516
GFX_ClearRegion  equ -528
GFX_DisposeRegion equ -534

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   done
    move.l  d0,a5
    move.l  4.w,a6
    lea     gfxname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,a6
    jsr     GFX_NewRegion(a6)
    tst.l   d0
    beq.s   failed
    move.l  d0,d6
    move.l  d6,a0
    jsr     GFX_DisposeRegion(a6)
    move.l  d6,a0
    jsr     GFX_ClearRegion(a6)
failed:
    lea     failmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)
done:
    moveq   #0,d0
    rts

dosname: dc.b "dos.library",0
gfxname: dc.b "graphics.library",0
failmsg: dc.b "[T3OSOBJ-BAD] FAIL: stale Region reached native code",10,0
