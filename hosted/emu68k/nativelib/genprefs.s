; genprefs.s - a whole AmigaOS value structure crossing, size-limited.
;
; GetPrefs is a raw CopyMem natively, so struct Preferences has to arrive in the
; guest with every field where a 68k program expects it and in the byte order it
; expects. Three things are needed that a flat scalar table does not have: the
; three struct timeval members have to be flattened to their leaf longs, the
; 36-entry PointerMatrix has to be converted element by element, and BOOL is two
; bytes, not four - taken for four it flattens PrinterType, which sits directly
; after it.
;
; The values are not asserted against constants. C:PrefsProbe reads the same
; fields natively in the same order and prints the same line, and the two are
; compared: whatever the system's preferences actually are, both sides must see
; them identically.
;
; Then the size argument, which is a bound and not a suggestion: a short call
; must fill exactly as far as it reaches and leave every byte beyond it alone.

EXEC_OpenLibrary  equ -552
EXEC_CloseLibrary equ -414
DOS_PutStr        equ -948
INT_GetPrefs      equ -132

SIZEOF_PREFS      equ 232
BUFSIZE           equ 240         ; the structure plus a margin of sentinel
SENTINEL          equ $5a
SHORTSIZE         equ 32          ; lands INSIDE PointerMatrix, on an element

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     done
    move.l  d0,a5

    move.l  4.w,a6
    lea     intname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     failed
    move.l  d0,a4

    ; ---- the whole structure ----------------------------------------------
    bsr     fillsentinel
    move.l  a4,a6
    lea     buffer(pc),a0
    move.l  a0,d7                   ; the buffer address, for the identity check
    moveq   #0,d0
    move.w  #SIZEOF_PREFS,d0
    jsr     INT_GetPrefs(a6)
    cmp.l   d7,d0                   ; GetPrefs returns the caller's own buffer
    bne     failed

    lea     buffer(pc),a0
    lea     report(pc),a1

    moveq   #0,d0                   ; FontHeight, a byte at 0
    move.b  (a0),d0
    bsr     hex8
    bsr     space
    move.l  8(a0),d0                ; KeyRptSpeed.tv_micro: a nested timeval leaf
    bsr     hex8
    bsr     space
    move.l  24(a0),d0               ; DoubleClick.tv_micro: the last before the array
    bsr     hex8
    bsr     space

    moveq   #0,d0                   ; the 36-entry pointer sprite, summed
    moveq   #35,d1
    lea     28(a0),a2
sumloop:
    moveq   #0,d2
    move.w  (a2)+,d2
    add.l   d2,d0
    dbra    d1,sumloop
    bsr     hex8
    bsr     space

    moveq   #0,d0                   ; PointerTicks: the first field AFTER the array
    move.w  108(a0),d0
    bsr     hex8
    bsr     space
    moveq   #0,d0                   ; EnableCLI: the two-byte BOOL
    move.w  124(a0),d0
    bsr     hex8
    bsr     space
    moveq   #0,d0                   ; PrinterType: what a four-byte BOOL would eat
    move.w  126(a0),d0
    bsr     hex8
    bsr     space
    move.l  128(a0),d0              ; PrinterFilename, the head of a byte block
    bsr     hex8
    bsr     space
    moveq   #0,d0                   ; ext_size: the very last byte
    move.b  231(a0),d0
    bsr     hex8
    move.b  #10,(a1)+
    clr.b   (a1)

    ; keep three values to compare after the short call. In memory, not in
    ; registers: they have to survive a library call, and what a crossing
    ; promises about d4-d7 is not what this test is here to prove.
    lea     buffer(pc),a0
    lea     saved(pc),a1
    move.b  (a0),(a1)               ; FontHeight
    move.w  28(a0),2(a1)            ; PointerMatrix[0]
    move.w  30(a0),4(a1)            ; PointerMatrix[1]

    lea     reportmsg(pc),a0
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)

    ; ---- a short call fills what it reaches, and nothing past it -----------
    bsr     fillsentinel
    move.l  a4,a6
    lea     buffer(pc),a0
    move.l  a0,d7
    moveq   #SHORTSIZE,d0
    jsr     INT_GetPrefs(a6)
    cmp.l   d7,d0
    bne     failed

    lea     buffer(pc),a0
    lea     saved(pc),a1
    move.b  (a1),d0
    cmp.b   (a0),d0                 ; inside the bound: same value as the full call
    bne     failed
    move.w  2(a1),d0
    cmp.w   28(a0),d0               ; the array fills as far as it reaches...
    bne     failed
    move.w  4(a1),d0
    cmp.w   30(a0),d0               ; ...element 1 ends exactly on the bound
    bne     failed

    lea     buffer(pc),a0
    lea     SHORTSIZE(a0),a0        ; every byte at or past the bound is untouched
    move.w  #BUFSIZE-SHORTSIZE-1,d1
tailloop:
    cmp.b   #SENTINEL,(a0)+
    bne     failed
    dbra    d1,tailloop

    ; ---- a zero-size call must write nothing -------------------------------
    bsr     fillsentinel
    move.l  a4,a6
    lea     buffer(pc),a0
    move.l  a0,d7
    moveq   #0,d0
    jsr     INT_GetPrefs(a6)
    cmp.l   d7,d0
    bne     failed
    lea     buffer(pc),a0
    move.w  #BUFSIZE-1,d1
zeroloop:
    cmp.b   #SENTINEL,(a0)+
    bne     failed
    dbra    d1,zeroloop

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

; ---- fill the buffer with a sentinel so an unwritten byte is recognisable ---
fillsentinel:
    lea     buffer(pc),a0
    move.w  #BUFSIZE-1,d0
fillloop:
    move.b  #SENTINEL,(a0)+
    dbra    d0,fillloop
    rts

; ---- d0 -> eight hex digits at (a1), a1 advanced. Clobbers d0, d2, d3.
; Upper case, because that is what the native oracle's %08lx prints and the two
; lines are compared as text. ------------------------------------------------
hex8:
    moveq   #7,d2
hexloop:
    rol.l   #4,d0
    move.l  d0,d3
    and.w   #$000f,d3
    cmp.w   #10,d3
    bge.s   hexalpha
    add.w   #'0',d3
    bra.s   hexput
hexalpha:
    add.w   #'A'-10,d3
hexput:
    move.b  d3,(a1)+
    dbra    d2,hexloop
    rts

space:
    move.b  #' ',(a1)+
    rts

dosname:  dc.b "dos.library",0
intname:  dc.b "intuition.library",0
passmsg:  dc.b "[T3PREF] PASS",10,0
failmsg:  dc.b "[T3PREF] FAIL",10,0
reportmsg:
          dc.b "[T3PREF-GUEST] "
report:   ds.b 96
    even
saved:    ds.b 8
buffer:   ds.b BUFSIZE
