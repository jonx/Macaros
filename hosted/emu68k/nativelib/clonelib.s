; clonelib.s - a 68k library that hands a DIFFERENT base to every opener.
;
; This is ordinary AmigaOS, not a curiosity: a library's Open vector returns the
; base the caller must use, and a library that keeps per-opener state returns a
; fresh copy of itself each time. The copy is made the classic way, from the
; library's own lib_NegSize/lib_PosSize, so the vector table below the base comes
; along with it and the clone is callable exactly like the original.
;
; What a loader has to get right, and what this exercises:
;   - the base a program holds is whatever Open returned, and CloseLibrary on it
;     must reach this library, not be rejected as an unknown base
;   - Close runs with A6 = the base being closed, or the wrong instance is freed
;   - two live opens are two references; closing one leaves the other working
;   - the last close still reaches the expunge path
;
; -30 CloneSerial -> D0 = which opener this base belongs to (1, 2, 3...)
; -36 CloneMagic  -> D0 = $c10ec0de

RTC_MATCHWORD   equ $4AFC
NT_LIBRARY      equ 9
RTF_AUTOINIT    equ $80
LIB_SIZE        equ 64

EXEC_AllocMem   equ -198
EXEC_FreeMem    equ -210
EXEC_CopyMem    equ -624
MEMF_PUBCLEAR   equ $00010001

LIB_NEGSIZE     equ 16          ; struct Library, the fields a clone is made from
LIB_POSSIZE     equ 18
LIB_OPENCNT     equ 32

SEGLIST_OFF     equ 40          ; master only
COUNT_OFF       equ 44          ; master only: opens outstanding, and the serial
SERIAL_OFF      equ 48          ; clone only
MASTER_OFF      equ 52          ; clone only: zero in the master itself

        moveq   #-1,d0
        rts

        cnop    0,4
romtag:
        dc.w    RTC_MATCHWORD
        dc.l    romtag
        dc.l    endskip
        dc.b    RTF_AUTOINIT
        dc.b    1
        dc.b    NT_LIBRARY
        dc.b    0
        dc.l    libname
        dc.l    libid
        dc.l    inittab

inittab:
        dc.l    LIB_SIZE
        dc.l    functable
        dc.l    0
        dc.l    initfunc

functable:
        dc.w    -1
        dc.w    libopen-functable
        dc.w    libclose-functable
        dc.w    libexpunge-functable
        dc.w    libreserved-functable
        dc.w    cloneserial-functable
        dc.w    clonemagic-functable
        dc.w    -1

; D0 = base, A0 = seglist, A6 = SysBase.
initfunc:
        move.l  d0,a1
        move.l  a0,SEGLIST_OFF(a1)
        clr.l   COUNT_OFF(a1)
        clr.l   MASTER_OFF(a1)          ; the master is nobody's clone
        rts

; ---- Open: build this opener's own base ------------------------------------
libopen:
        movem.l d2-d4/a2-a3,-(sp)
        move.l  a6,a3                   ; a3 = the master base
        moveq   #0,d2
        move.w  LIB_NEGSIZE(a3),d2
        moveq   #0,d3
        move.w  LIB_POSSIZE(a3),d3
        move.l  d2,d4
        add.l   d3,d4                   ; d4 = vectors + structure

        move.l  d4,d0
        move.l  #MEMF_PUBCLEAR,d1
        move.l  4.w,a6
        jsr     EXEC_AllocMem(a6)
        tst.l   d0
        beq.s   openfailed
        move.l  d0,a2                   ; a2 = the allocation, vectors first

        move.l  a3,a0
        sub.l   d2,a0                   ; from the master's own vector table
        move.l  a2,a1
        move.l  d4,d0
        move.l  4.w,a6
        jsr     EXEC_CopyMem(a6)

        add.l   d2,a2                   ; a2 = the clone BASE
        addq.l  #1,COUNT_OFF(a3)
        move.l  COUNT_OFF(a3),SERIAL_OFF(a2)
        move.l  a3,MASTER_OFF(a2)
        move.w  #1,LIB_OPENCNT(a2)
        move.l  a2,d0
        movem.l (sp)+,d2-d4/a2-a3
        rts
openfailed:
        moveq   #0,d0
        movem.l (sp)+,d2-d4/a2-a3
        rts

; ---- Close: A6 is the base being closed, which is one specific clone --------
libclose:
        movem.l d2-d3/a2-a3,-(sp)
        move.l  a6,a2
        move.l  MASTER_OFF(a2),d0
        beq.s   closemaster             ; the master itself was never handed out
        move.l  d0,a3

        moveq   #0,d2
        move.w  LIB_NEGSIZE(a3),d2
        moveq   #0,d3
        move.w  LIB_POSSIZE(a3),d3
        move.l  a2,a1
        sub.l   d2,a1                   ; back to the allocation start
        move.l  d2,d0
        add.l   d3,d0
        move.l  4.w,a6
        jsr     EXEC_FreeMem(a6)

        subq.l  #1,COUNT_OFF(a3)
        move.l  COUNT_OFF(a3),d0
        bne.s   closestill
        move.l  SEGLIST_OFF(a3),d0      ; last one out expunges
        movem.l (sp)+,d2-d3/a2-a3
        rts
closestill:
        moveq   #0,d0
        movem.l (sp)+,d2-d3/a2-a3
        rts
closemaster:
        moveq   #0,d0
        movem.l (sp)+,d2-d3/a2-a3
        rts

libexpunge:
        move.l  SEGLIST_OFF(a6),d0
        rts
libreserved:
        moveq   #0,d0
        rts

cloneserial:
        move.l  SERIAL_OFF(a6),d0
        rts
clonemagic:
        move.l  #$c10ec0de,d0
        rts

libname:
        dc.b    "clone.library",0
libid:
        dc.b    "clone.library 1.0 (2026)",13,10,0
        cnop    0,4
endskip:
