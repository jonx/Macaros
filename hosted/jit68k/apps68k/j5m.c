/* j5m.c — a SELF-CONTAINED compute-heavy + output C program, compiled by the
 * from-source vbcc m68k cross-compiler (tools/build-vbcc.sh) to a REAL big-endian
 * AmigaOS hunk executable, and run THROUGH the 68k->AArch64 JIT.  This is the
 * milestone: COMPILER-generated 68k code (movem prologues, the full AmigaOS calling
 * convention, stack frames, jsr/bsr, Bcc) executing on Apple silicon via the JIT,
 * byte-exact against the independent interpreter.
 *
 * NO Amiga SDK, NO real libc.  Everything below is hand-written:
 *   - the entry/exit are in crt0.s (entry -> main -> exit code in d0 -> rts);
 *   - the ONLY OS service used is PutChar (exec LVO -30), reached through the tiny
 *     putch() shim in crt0.s, which does the classic negative-offset `jsr -30(a6)`
 *     LVO call.  A6 is the library base, already seeded by the engine (== the runner's
 *     stub exec base).  So all output flows through the EXISTING [J3] LVO bridge into
 *     the stub PutChar sink — the same path libcall.s/mandel.s use.
 *
 * The program is INTEGER-ONLY (-no-fp): the toolchain was built with host-native
 * datatypes (build-vbcc.sh answers dtgen cross=n), which is correct for integer
 * constant folding but NOT for FP on a little-endian host, and the JIT engine is
 * integer-user-mode anyway (spec [J5] scope).  So: no float/double anywhere.
 *
 * What it exercises (real C the compiler lowers to the full m68k convention):
 *   - puts / a hand-written unsigned + signed integer-to-decimal printer (puti/putu)
 *     -> div/mod loops the compiler lowers to muls/divu (LINEC/MULDIV);
 *   - an ITERATIVE Fibonacci loop and a RECURSIVE Fibonacci (deep jsr/bsr nesting,
 *     movem.l prologue/epilogue around each call, the return stack);
 *   - a FACTORIAL table (32-bit overflow wraps, exercising multiply);
 *   - an in-place BUBBLE SORT of an int array (array indexing -> (d8,An,Xn) /
 *     (d16,An) EAs, compares, swaps), then printed;
 *   - a final 32-bit CHECKSUM folded across all of it, returned from main as the
 *     exit code so the run has a single value-asserted result in d0.
 */

/* ---- the one OS service: print a single character (exec PutChar, LVO -30) -------
 * Implemented in crt0.s as `jsr -30(a6)` (the classic AmigaOS negative-offset LVO
 * call).  We just declare it here; the linker resolves it to the asm shim. */
extern void putch(int c);

/* ---- tiny output helpers (no libc) --------------------------------------------- */
static void puts_(const char *s)
{
    while (*s) putch((int)(unsigned char)*s++);
}

/* unsigned decimal */
static void putu(unsigned long v)
{
    char buf[16];
    int n = 0;
    if (v == 0) { putch('0'); return; }
    while (v) { buf[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) putch(buf[--n]);
}

/* signed decimal */
static void puti(long v)
{
    if (v < 0) { putch('-'); putu((unsigned long)(-v)); }
    else        putu((unsigned long)v);
}

static void nl(void) { putch('\n'); }

/* ---- compute: iterative + recursive Fibonacci ---------------------------------- */
static unsigned long fib_iter(int n)
{
    unsigned long a = 0, b = 1, t;
    int i;
    for (i = 0; i < n; i++) { t = a + b; a = b; b = t; }
    return a;
}

static unsigned long fib_rec(int n)        /* deep recursion: jsr/bsr nesting */
{
    if (n < 2) return (unsigned long)n;
    return fib_rec(n - 1) + fib_rec(n - 2);
}

/* ---- compute: factorial (32-bit, wraps) ---------------------------------------- */
static unsigned long factorial(int n)
{
    unsigned long f = 1;
    int i;
    for (i = 2; i <= n; i++) f *= (unsigned long)i;
    return f;
}

/* ---- compute: in-place bubble sort --------------------------------------------- */
static void bubble_sort(long *a, int n)
{
    int i, j;
    for (i = 0; i < n - 1; i++)
        for (j = 0; j < n - 1 - i; j++)
            if (a[j] > a[j + 1]) {
                long t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;
            }
}

int main(void)
{
    unsigned long checksum = 0;
    int i;

    /* (1) iterative Fibonacci table 0..15 */
    puts_("fib(iter):");
    for (i = 0; i <= 15; i++) {
        unsigned long f = fib_iter(i);
        putch(' '); putu(f);
        checksum = checksum * 31u + f;
    }
    nl();

    /* (2) recursive Fibonacci — must agree with the iterative one */
    puts_("fib(rec): ");
    for (i = 0; i <= 12; i++) {
        unsigned long f = fib_rec(i);
        putch(' '); putu(f);
        checksum = checksum * 31u + f;
    }
    nl();

    /* (3) factorial table 1..12 (12! still fits in 32 bits) */
    puts_("fact:");
    for (i = 1; i <= 12; i++) {
        unsigned long f = factorial(i);
        putch(' '); putu(f);
        checksum = checksum * 31u + f;
    }
    nl();

    /* (4) bubble sort of an unsorted array, then print + fold in */
    {
        long arr[10];
        long seed = 12345;
        /* fill with a simple LCG so the data is data-dependent, not constant-folded */
        for (i = 0; i < 10; i++) {
            seed = (seed * 1103515245L + 12345L) & 0x7fffffffL;
            arr[i] = seed % 1000;
        }
        bubble_sort(arr, 10);
        puts_("sorted:");
        for (i = 0; i < 10; i++) {
            putch(' '); puti(arr[i]);
            checksum = checksum * 31u + (unsigned long)arr[i];
        }
        nl();
        /* the array must be non-decreasing after the sort — fold a 1 per ordered pair */
        for (i = 0; i < 9; i++)
            if (arr[i] <= arr[i + 1]) checksum += 1u;
    }

    puts_("checksum=");
    putu(checksum);
    nl();

    /* return the low 16 bits of the checksum as the program exit code (d0). A single
     * value the JIT run and the interpreter must agree on, on top of the byte-exact
     * register/memory/PutChar-stream comparison. */
    return (int)(checksum & 0xffffu);
}
