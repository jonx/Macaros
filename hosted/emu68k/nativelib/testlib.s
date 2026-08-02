; testlib.s - a minimal AmigaOS 68k .library, as a TEST ARTIFACT.
;
; Running third-party libraries in the guest ([T3e]) could not be built because
; there was no 68k .library on this machine to test it against, and building a
; loader with nothing to load it is how you get code that compiles and has
; never been right. This is that artifact: a real hunk file with a real
; resident tag and a real init routine, small enough to reason about entirely.
;
; What a loader has to find here, in order:
;   - the hunk file loads as ordinary guest memory
;   - RTC_MATCHWORD ($4AFC) whose rt_MatchTag points AT ITSELF (that self-
;     reference is what distinguishes a tag from the same two bytes appearing
;     in code by accident - $4AFC is the ILLEGAL instruction and turns up)
;   - rt_Type = NT_LIBRARY (9), rt_Flags = 0 (direct initialization)
;   - rt_Init, entered with A0 = seglist, D0 = 0, A4 = 0, A6 = SysBase, returning the
;     library base in D0 - a GUEST address, which is the whole point: a native
;     base could never be handed to a 68k program
;
; The library it builds answers two vectors so a loader can be proven end to
; end rather than just "it didn't crash":
;   -30  TestAdd(D0,D1) -> D0 = D0 + D1
;   -36  TestMagic()    -> D0 = $5AFEC0DE
;
; Deliberately NOT calling exec's MakeLibrary: that would make this a test of
; exec rather than of the loader. The vector table is laid out by hand, which
; is also what makes the expected addresses checkable by eye.

RTC_MATCHWORD   equ $4AFC
NT_LIBRARY      equ 9
; ---- hunk entry. A library loaded with LoadSeg starts with a MOVEQ #-1 so
; that anyone who mistakenly RUNS it exits with an error instead of executing
; the resident structure as code. This is the standard first word of every
; Amiga library.
        moveq   #-1,d0
        rts

; ---- the resident tag ------------------------------------------------------
        cnop    0,4
romtag:
        dc.w    RTC_MATCHWORD           ; rt_MatchWord
        dc.l    romtag                  ; rt_MatchTag -> ITSELF
        dc.l    endskip                 ; rt_EndSkip
        dc.b    0                       ; rt_Flags: rt_Init is executable code
        dc.b    1                       ; rt_Version
        dc.b    NT_LIBRARY              ; rt_Type
        dc.b    0                       ; rt_Pri
        dc.l    libname                 ; rt_Name
        dc.l    libid                   ; rt_IdString
        dc.l    init                    ; rt_Init

; ---- init: A0 = seglist, D0 = 0, A6 = SysBase; returns the base in D0 -------
; The base is this library's own data area, IN THE GUEST. The vector table sits
; immediately below it, at negative offsets, exactly as a real library does, so
; the engine's existing "recognise a vector by the address it lands on" works
; without knowing anything about this file.
init:
        lea     libbase(pc),a0
        move.l  a0,d0                   ; D0 = the base, a guest address
        rts

; ---- the vectors, laid out below the base ----------------------------------
; -36  TestMagic
; -30  TestAdd
        cnop    0,4
vectors:
        jmp     testmagic(pc)           ; base-36
        dc.w    0
        jmp     testadd(pc)             ; base-30
        dc.w    0
        jmp     libreserved(pc)         ; base-24
        dc.w    0
        jmp     libexpunge(pc)          ; base-18
        dc.w    0
        jmp     libclose(pc)            ; base-12
        dc.w    0
        jmp     libopen(pc)             ; base-6
        dc.w    0
libbase:
        ds.b    64                      ; a minimal struct Library area

libopen:
        move.l  a6,d0
        rts

libclose:
libexpunge:
libreserved:
        moveq   #0,d0
        rts

testadd:
        add.l   d1,d0
        rts

testmagic:
        move.l  #$5afec0de,d0
        rts

libname:
        dc.b    "test.library",0
libid:
        dc.b    "test.library 1.0 (2026)",13,10,0
        cnop    0,4
endskip:
