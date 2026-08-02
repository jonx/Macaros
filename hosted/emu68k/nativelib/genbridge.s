; genbridge.s - exercises the GENERATED half of the library bridge.
;
; Every vector this program calls is a tier-1 crossing derived from the .conf
; files by graft/gen-emu68k-bridge, and NONE of them is hand-written in
; emu68k_oscall.c. So if this prints PASS, the generated table is doing real
; work: arguments in D registers and in A registers, a second library opened on
; demand, and results that are checked rather than merely returned.
;
;   dos.ParsePattern   -840  (D1,D2,D3)  builds a tokenised pattern
;   dos.MatchPattern   -846  (D1,D2)     must match, then must NOT match
;   utility.Stricmp    -162  (A0,A1)     A-register arguments, second library
;   dos.PutStr         -948  (D1)        prints the verdict through the table

EXEC_OpenLibrary    equ -552
EXEC_CloseLibrary   equ -414
DOS_ParsePattern    equ -840
DOS_MatchPattern    equ -846
DOS_PutStr          equ -948
UTIL_Stricmp        equ -162

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

    lea     okmsg(pc),a0
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
pat:      dc.b "#?.txt",0
good:     dc.b "hello.txt",0
bad:      dc.b "hello.doc",0
s1:       dc.b "AbCdEf",0
s2:       dc.b "aBcDeF",0
s3:       dc.b "zzz",0
okmsg:    dc.b "[T3GEN] PASS",10,0
nopemsg:  dc.b "[T3GEN] FAIL",10,0
    even
patbuf:   ds.b 64
