; crt0_args.s — C runtime startup that DELIVERS the AmigaDOS CLI arguments to main()
; as a C `int main(int argc, char **argv)`.  Hand-written (OURS); NO Amiga SDK, NO real
; libc.  Assembled by vasm (mot syntax), linked FIRST (so _start is the entry / first
; byte of the CODE hunk, per the [J4] loader / engine entry contract) with a vbcc-
; compiled main (apps68k/args.c) by vlink into one AmigaOS hunk executable.
;
; ============================ THE AMIGADOS ENTRY CONVENTION ============================
; A CLI-launched AmigaOS program is entered (by run68k, honouring the convention) with:
;     A0 = pointer to the ARGUMENT STRING — the command line AFTER the command name (the
;          program name is NOT in it).  The string ends with a newline '\n'.
;     D0 = the LENGTH of that string in bytes, INCLUDING the terminating '\n'.
; No args  ->  the string is just "\n" and D0 = 1.
; This startup reads A0/D0, splits the string into whitespace-separated tokens, builds a
; NUL-terminated argv[] array, and calls main(argc, argv).
;
; ============================ THE SPLITTER (documented semantics) ======================
; A simple, deterministic whitespace splitter:
;   * whitespace = space (0x20), tab (0x09) and the terminating newline (0x0A).
;   * a TOKEN is a maximal run of NON-whitespace bytes.  Runs of whitespace are collapsed
;     (multiple spaces between tokens produce no empty args).  Leading/trailing whitespace
;     is skipped.  The '\n' that ends the string ends the last token / the scan.
;   * NO quote grouping: run68k already did the AmigaDOS join (single-space-joined the
;     program-args the host shell handed it); this splitter re-splits that joined string on
;     whitespace.  So a shell-quoted "a b" — which the host shell passed as ONE program-arg —
;     was joined by run68k into the same one string as the unquoted a b, and this splitter
;     yields the same two tokens.  (Documented in run68k.md: shell quoting vs AmigaDOS
;     quoting differ; AmigaDOS-level quote handling is a deliberate non-goal here.)
;
; ============================ argv[0] (documented choice) ==============================
; The AmigaDOS arg string does NOT contain the command name (the convention excludes it),
; so this startup synthesises argv[0] = a fixed placeholder "a.out".  The ACTUAL passed
; args land in argv[1..argc-1], exactly the C convention (argv[0] = program name, argv[1..]
; = the args).  argc therefore counts the placeholder + the tokens.  (run68k could pass the
; real basename via a small extension; not done — the placeholder is documented and stable.)
;
; ============================ vbcc CALLING CONVENTION (verified) =======================
; vbcc's m68k convention (same as crt0.s, verified from its codegen): integer/pointer args
; are PUSHED on the stack by the CALLER, right-to-left, so the callee reads the 1st arg at
; 4(a7) and the 2nd at 8(a7) (4 = the return address jsr pushed).  Return value in d0.  So
; to call main(argc, argv) we push argv THEN argc (right-to-left), jsr _main, and use main's
; d0 return as the program exit code (the top-level RTS = program exit, d0 = exit code).
;
; ============================ THE PutChar SHIM (output sink) ===========================
; Same classic AmigaOS negative-offset LVO call as crt0.s: PutChar is exec LVO 5 -> byte
; offset -30, char in d0, A6 = library base (the engine seeds A6 at entry).  args.c calls
; putch() through this shim, so its output flows through the same [J3] LVO bridge as the
; rest of the corpus -> the stub PutChar sink -> run68k stdout.

LVO_PutChar     equ     -30             ; exec PutChar: -5*6

; ----- Sandbox-resident scratch for the argv[] machinery (OURS) -----------------------
; run68k places the AmigaDOS arg STRING at 0x00238000 (the free gap between the AllocMem
; heap end 0x238000 and the [J5i] vector table 0x240000; capped to 4096 bytes, so the
; string ends well before 0x239000).  We build the argv[] pointer array and the argv[0]
; placeholder string HIGHER in that same 32 KiB region, so nothing collides:
;   - ARGV_ARRAY : the array of 32-bit argv pointers (argv[0..])      @ 0x0023C000
;   - ARGV0_STR  : the NUL-terminated "a.out" placeholder for argv[0] @ 0x0023E000
; Both sit inside [0x238000, 0x240000), untouched by the loader/heap/vectors/arg-string.
ARGV_ARRAY      equ     $0023C000
ARGV0_STR       equ     $0023E000
MAX_ARGV        equ     64              ; cap on argc (incl. argv[0]); array is 64*4 bytes

        section "CODE",code

        public  _start
_start:
        ; ---- on entry: a0 = arg string ptr, d0 = length (incl '\n') ----
        ; a0 = current scan pointer ; a1 = one-past-the-end of the string
        ; a2 = argv[] write cursor  ; d2 = argc
        movea.l a0,a1
        add.l   d0,a1               ; a1 = a0 + length = end of the string (exclusive)
        movea.l #ARGV_ARRAY,a2      ; a2 -> argv[] array

        ; ---- argv[0] = the "a.out" placeholder (synthesised; the arg string has no name) ----
        bsr     _make_argv0         ; writes "a.out\0" at ARGV0_STR
        move.l  #ARGV0_STR,(a2)+    ; argv[0] = &"a.out"
        moveq   #1,d2               ; argc = 1 so far (the placeholder)

.scan:
        ; ---- skip leading/inter-token whitespace ----
.skipws:
        cmpa.l  a1,a0               ; reached the end?
        bge.s   .done
        move.b  (a0),d1             ; d1.b = current char (sign-extended; we mask below)
        and.l   #$FF,d1
        cmp.l   #$20,d1             ; space?
        beq.s   .ws
        cmp.l   #$09,d1             ; tab?
        beq.s   .ws
        cmp.l   #$0A,d1             ; newline (the terminator)?
        beq.s   .ws
        bra.s   .tokstart           ; non-whitespace -> a token begins here
.ws:
        addq.l  #1,a0
        bra.s   .skipws

.tokstart:
        ; ---- record this token's start, then advance to its end ----
        cmp.l   #MAX_ARGV,d2        ; argv[] full? (defensive cap)
        bge.s   .done
        move.l  a0,(a2)+            ; argv[argc] = start of token
        addq.l  #1,d2               ; argc++
.tokscan:
        cmpa.l  a1,a0               ; end of string?
        bge.s   .tokend
        move.b  (a0),d1
        and.l   #$FF,d1
        cmp.l   #$20,d1             ; whitespace ends the token
        beq.s   .tokend
        cmp.l   #$09,d1
        beq.s   .tokend
        cmp.l   #$0A,d1
        beq.s   .tokend
        addq.l  #1,a0               ; still inside the token
        bra.s   .tokscan
.tokend:
        ; NUL-terminate the token in place (overwrite the whitespace byte with 0), then
        ; step past it and continue scanning for the next token.
        cmpa.l  a1,a0               ; (don't write past the end; a0==a1 means end-of-string)
        bge.s   .done
        clr.b   (a0)                ; token[len] = 0
        addq.l  #1,a0
        bra.s   .scan

.done:
        ; ---- call main(argc, argv): push argv then argc (right-to-left), jsr _main ----
        move.l  #ARGV_ARRAY,-(a7)   ; arg 2 = argv  (pushed first = higher address)
        move.l  d2,-(a7)            ; arg 1 = argc
        jsr     _main
        addq.l  #8,a7               ; caller cleans the 2 pushed args (8 bytes)
        ; main's int return is already in d0 (vbcc convention) -> the exit code
        rts                         ; top-level RTS = program exit (d0 = exit code)

; ----- _make_argv0: write the NUL-terminated placeholder "a.out" at ARGV0_STR ----------
; Five ASCII bytes 'a','.','o','u','t' + NUL.  Written byte-by-byte (no DATA hunk reloc
; needed; the engine handles move.b #imm,(An)+).
_make_argv0:
        movea.l #ARGV0_STR,a3
        move.b  #$61,(a3)+          ; 'a'
        move.b  #$2E,(a3)+          ; '.'
        move.b  #$6F,(a3)+          ; 'o'
        move.b  #$75,(a3)+          ; 'u'
        move.b  #$74,(a3)+          ; 't'
        clr.b   (a3)               ; '\0'
        rts

; ----- void putch(int c);  c at 4(a7) on entry.  Marshal to d0, call PutChar(a6) --------
        public  _putch
_putch:
        move.l  4(a7),d0            ; d0 = c  (PutChar's char arg, exec ABI: D0)
        jsr     LVO_PutChar(a6)     ; jsr -30(a6) -> [J3] bridge -> stub PutChar
        rts
