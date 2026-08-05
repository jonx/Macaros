; genlibsweep.s - the whole tail set in one run.
;
; Opens every library we intend to run above the waterline and reports one
; line each. Routed guest-side (EMU68K_GUESTSIDE_LIBS), a line says the m68k
; binary LOADED, relocated and answered its resident init - which is the
; per-library fact that has to be true before behaviour is worth testing, and
; the fact a loader gap (a hunk type, a reserved vector) breaks. One run
; covers the set, so a loader change is verified across all of them at once
; rather than one library at a time.

EXEC_OpenLibrary   equ -552
EXEC_CloseLibrary  equ -414
DOS_PutStr         equ -948

    move.l  4.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq.w   done
    move.l  d0,a5                     ; a5 = DOSBase

    lea     names(pc),a4              ; a4 = walk of NUL-terminated names
loop:
    tst.b   (a4)
    beq.w   done

    move.l  4.w,a6
    move.l  a4,a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    move.l  d0,d7                     ; d7 = library base or 0

    ; "<name>: " then OK/FAIL
    move.l  a5,a6
    move.l  a4,d1
    jsr     DOS_PutStr(a6)
    lea     sep(pc),a0
    move.l  a0,d1
    jsr     DOS_PutStr(a6)

    tst.l   d7
    beq.b   sayfail
    lea     okmsg(pc),a0
    bra.b   saydone
sayfail:
    lea     failmsg(pc),a0
saydone:
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)

    tst.l   d7
    beq.b   nextname
    move.l  4.w,a6
    move.l  d7,a1
    jsr     EXEC_CloseLibrary(a6)

nextname:
    ; advance past this name's terminator
skip:
    tst.b   (a4)+
    bne.b   skip
    bra.w   loop

done:
    moveq   #0,d0
    rts

dosname:  dc.b "dos.library",0
sep:      dc.b ": ",0
okmsg:    dc.b "LOADED",10,0
failmsg:  dc.b "no",10,0

names:
    dc.b "gadtools.library",0
    dc.b "iffparse.library",0
    dc.b "locale.library",0
    dc.b "icon.library",0
    dc.b "datatypes.library",0
    dc.b "coolimages.library",0
    dc.b "asl.library",0
    dc.b "diskfont.library",0
    dc.b "commodities.library",0
    dc.b "muimaster.library",0
    dc.b 0
    even
