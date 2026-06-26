/* args.c — a SELF-CONTAINED C program that PRINTS its command-line arguments, compiled
 * by the from-source vbcc m68k cross-compiler and run THROUGH the 68k->AArch64 JIT.  It
 * proves run68k delivers the AmigaDOS CLI argument string into the program: the startup
 * (crt0_args.s) reads A0/D0, splits the string into argv[], and calls main(argc, argv);
 * this main prints argc and each argv[i], one per line, through the stub PutChar sink.
 *
 * NO Amiga SDK, NO real libc.  The ONLY OS service is PutChar (exec LVO -30), reached
 * through the putch() shim in crt0_args.s (the classic negative-offset `jsr -30(a6)` LVO
 * call), so all output flows through the EXISTING [J3] LVO bridge -> stub PutChar ->
 * run68k stdout, exactly like j5m.c / mandel.s.
 *
 * Integer-only (-no-fp): no float/double anywhere (the toolchain's integer datatypes; the
 * JIT integer-user-mode scope).  argv[0] is the synthesised placeholder "a.out" (the
 * AmigaDOS arg string excludes the command name — see crt0_args.s); the actual passed
 * args are argv[1..argc-1].
 */

extern void putch(int c);                  /* the one OS service (PutChar LVO -30) */

static void puts_(const char *s)
{
    while (*s) putch((int)(unsigned char)*s++);
}

/* unsigned decimal (no libc) */
static void putu(unsigned long v)
{
    char buf[16];
    int n = 0;
    if (v == 0) { putch('0'); return; }
    while (v) { buf[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) putch(buf[--n]);
}

static void nl(void) { putch('\n'); }

int main(int argc, char **argv)
{
    int i;

    /* argc on its own line, prefixed, so the result is unambiguous on stdout. */
    puts_("argc=");
    putu((unsigned long)argc);
    nl();

    /* each argument, one per line: "argv[i]=<value>".  argv[0] is the "a.out" placeholder;
     * the passed args are argv[1..]. */
    for (i = 0; i < argc; i++) {
        puts_("argv[");
        putu((unsigned long)i);
        puts_("]=");
        puts_(argv[i]);
        nl();
    }

    /* return argc as the exit code (clamped 0..255 by run68k) so the run has a single
     * value-asserted result in d0 on top of the printed stream. */
    return argc;
}
