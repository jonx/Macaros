; diagbus.s — [J5n] a 68k program that jumps to a PC OUTSIDE the sandbox with no bus-error
; handler installed, so it is an UNRECOVERABLE 68k fault (vector 2) -> the diagnostics bundle.
; This is the hosted stand-in for the host SIGSEGV an integrated JIT would take (the
; graft/cpu_aarch64.h seam). Assembled to a big-endian AmigaOS hunk executable.
;
;   start:
;       moveq #1,d0
;       jmp   $00100000      ; below the sandbox origin -> 68k bus error, no handler -> bundle

    moveq   #1,d0
    jmp     $00100000
