; genbridge.s - exercises the GENERATED half of the library bridge.
;
; Every vector this program calls is generated from the .conf files by
; graft/gen-emu68k-bridge, and NONE is hand-written in emu68k_oscall.c. The
; BestModeIDA call additionally consumes the type policy: its guest-layout
; TagItem chain is flattened and rebuilt in native layout before graphics sees
; it. So PASS covers both free tier-1 generation and the first tier-2 compiler.
;
;   dos.ParsePattern   -840  (D1,D2,D3)  builds a tokenised pattern
;   dos.MatchPattern   -846  (D1,D2)     must match, then must NOT match
;   dos.DateStamp      -192  (D1)        generated OUT value-structure shadow
;   dos.CompareDates   -738  (D1,D2)     two generated IN structure shadows
;   utility.Stricmp    -162  (A0,A1)     A-register arguments, second library
;   graphics.BestModeIDA -1050 (A0)      policy-typed TagItem shadow
;   cybergraphics.BestCModeIDTagList -60  string-valued tag data
;   dos.PutStr         -948  (D1)        prints the verdict through the table

EXEC_OpenLibrary    equ -552
EXEC_CloseLibrary   equ -414
DOS_ParsePattern    equ -840
DOS_MatchPattern    equ -846
DOS_PutStr          equ -948
DOS_DateStamp       equ -192
DOS_CompareDates    equ -738
UTIL_Stricmp        equ -162
GFX_BestModeIDA     equ -1050
CGFX_BestCModeID    equ -60

    move.l  4.w,a6                  ; SysBase from absolute address 4
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     nope
    move.l  d0,a5                   ; a5 = DOSBase

    ; ---- ParsePattern("#?.txt", patbuf, 64) --------------------------------
    move.l  a5,a6
    lea     pat(pc),a0
    move.l  a0,d1
    lea     patbuf(pc),a0
    move.l  a0,d2
    moveq   #64,d3
    jsr     DOS_ParsePattern(a6)
    tst.l   d0
    bmi     nope                    ; -1 = buffer too small / bad pattern

    ; ---- MatchPattern(patbuf, "hello.txt") must be TRUE ---------------------
    lea     patbuf(pc),a0
    move.l  a0,d1
    lea     good(pc),a0
    move.l  a0,d2
    jsr     DOS_MatchPattern(a6)
    tst.l   d0
    beq     nope                    ; a name that should match, did not

    ; ---- MatchPattern(patbuf, "hello.doc") must be FALSE --------------------
    lea     patbuf(pc),a0
    move.l  a0,d1
    lea     bad(pc),a0
    move.l  a0,d2
    jsr     DOS_MatchPattern(a6)
    tst.l   d0
    bne     nope                    ; a name that should NOT match, did

    ; ---- DateStamp OUT shadow: native fields copy back as guest big-endian -
    move.l  a5,a6
    lea     nowstamp(pc),a0
    move.l  a0,d1
    jsr     DOS_DateStamp(a6)
    cmp.l   d1,d0                   ; pointer result maps back to the guest arg
    bne     dateptrfail
    move.l  (a0),d4
    cmp.l   #$11223344,d4           ; OUT copy must replace the sentinel even
                                    ; when this boot has an unset (all-zero) date
    beq     datecopyfail

    ; ---- CompareDates: two guest structs become two native structs ---------
    lea     laterstamp(pc),a0
    move.l  a0,d1
    lea     earlierstamp(pc),a0
    move.l  a0,d2
    jsr     DOS_CompareDates(a6)
    tst.l   d0                      ; date1 later => negative (Amiga ordering)
    bpl     datecmpfail

    ; ---- utility.library, opened on demand by the generated table -----------
    move.l  4.w,a6
    lea     utilname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     nope
    move.l  d0,a4                   ; a4 = UtilityBase

    ; Stricmp("AbCdEf","aBcDeF") == 0, arguments in A0/A1 not D registers
    move.l  a4,a6
    lea     s1(pc),a0
    lea     s2(pc),a1
    jsr     UTIL_Stricmp(a6)
    tst.l   d0
    bne     nope

    ; Stricmp("AbCdEf","zzz") != 0, so a pass is not just a stuck zero
    move.l  a4,a6
    lea     s1(pc),a0
    lea     s3(pc),a1
    jsr     UTIL_Stricmp(a6)
    tst.l   d0
    beq     nope

    move.l  a4,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)

    ; ---- graphics.BestModeIDA: a real policy-compiled TagItem crossing -----
    move.l  4.w,a6
    lea     gfxname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     nope
    move.l  d0,a3

    ; The first list exercises TAG_IGNORE and TAG_SKIP (the skipped tag is
    ; deliberately unknown), then TAG_MORE chains to the actual scalar domain.
    move.l  a3,a6
    lea     besttags(pc),a0
    jsr     GFX_BestModeIDA(a6)
    cmp.l   #-1,d0                 ; INVALID_ID means no native mode was found
    beq     nope

    move.l  a3,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)

    ; ---- string-valued tag data: guest CSTR becomes a native pointer --------
    move.l  4.w,a6
    lea     cgfxname(pc),a1
    moveq   #0,d0
    jsr     EXEC_OpenLibrary(a6)
    tst.l   d0
    beq     nope
    move.l  d0,a2

    ; Asking for a deliberately nonexistent board must reach native cgfx and
    ; return INVALID_ID. A stale D0 or an unserved crossing cannot satisfy it.
    move.l  a2,a6
    moveq   #0,d0
    lea     cgfxtags(pc),a0
    jsr     CGFX_BestCModeID(a6)
    cmp.l   #-1,d0
    bne     nope

    move.l  a2,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)

    lea     okmsg(pc),a0
    bra.s   say
dateptrfail:
    lea     dateptrmsg(pc),a0
    bra.s   say
datecopyfail:
    lea     datecopymsg(pc),a0
    bra.s   say
datecmpfail:
    lea     datecmpmsg(pc),a0
    bra.s   say
nope:
    lea     nopemsg(pc),a0
say:
    move.l  a0,d1
    move.l  a5,a6
    jsr     DOS_PutStr(a6)

    move.l  a5,a1
    move.l  4.w,a6
    jsr     EXEC_CloseLibrary(a6)
    moveq   #0,d0
    rts

dosname:  dc.b "dos.library",0
utilname: dc.b "utility.library",0
gfxname:  dc.b "graphics.library",0
cgfxname: dc.b "cybergraphics.library",0
pat:      dc.b "#?.txt",0
good:     dc.b "hello.txt",0
bad:      dc.b "hello.doc",0
s1:       dc.b "AbCdEf",0
s2:       dc.b "aBcDeF",0
s3:       dc.b "zzz",0
okmsg:    dc.b "[T3GEN] PASS",10,0
nopemsg:  dc.b "[T3GEN] FAIL",10,0
dateptrmsg: dc.b "[T3GEN] FAIL DateStamp pointer",10,0
datecopymsg: dc.b "[T3GEN] FAIL DateStamp copy",10,0
datecmpmsg: dc.b "[T3GEN] FAIL CompareDates",10,0
    even
besttags:
    dc.l 1,0                         ; TAG_IGNORE
    dc.l 3,1                         ; TAG_SKIP one following item
    dc.l $8fffffff,$12345678         ; unknown, but skipped by TAG_SKIP
    dc.l 2,bestmore                  ; TAG_MORE
bestmore:
    dc.l $80000006,640               ; BIDTAG_DesiredWidth
    dc.l $80000007,480               ; BIDTAG_DesiredHeight
    dc.l $80000008,8                 ; BIDTAG_Depth
    dc.l 0,0                         ; TAG_DONE
cgfxtags:
    dc.l $80050005,nosuchboard       ; CYBRBIDTG_BoardName (CSTR)
    dc.l 0,0
nosuchboard: dc.b "emu68k-no-such-board",0
    even
nowstamp:     dc.l $11223344,$55667788,$10203040
laterstamp:   dc.l 2,10,5
earlierstamp: dc.l 1,10,5
patbuf:   ds.b 64
