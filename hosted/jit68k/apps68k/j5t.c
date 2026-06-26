/* j5t.c — THE FP CAPSTONE: a self-contained C program that does REAL floating-point
 * work in `double`, compiled ON THIS MAC by the from-source vbcc m68k cross-compiler
 * with HARDWARE 68881/68882 FP codegen (-fpu=68881), and run THROUGH the 68k->AArch64
 * JIT byte-exact vs the independent interpreter.  This is the FP analog of the [J5m]
 * integer capstone — COMPILER-generated 68k FP code (FMOVE/FADD/FMUL/FDIV/FCMP/FBcc/
 * FMOVEM, plus the hardware transcendentals FSQRT/FSIN/FETOX) executing on Apple
 * silicon via the JIT, closing out the FPU goal ([J5o]-[J5t]).
 *
 * HOW THE COMPILER EMITS HARDWARE FP (the key config, see tools/compile-j5t.sh):
 *   vbcc -cpu=68020 -fpu=68881  ->  `float`/`double` arithmetic lowers to LINE-F FP
 *   instructions, NOT soft-float library calls.  The toolchain's vbcc is rebuilt with
 *   dtgen cross=y (tools/build-vbcc.sh) so FP CONSTANTS are emitted big-endian
 *   (IEEE-754 target byte order) — the cross=n build byte-swapped them.
 *
 * NO Amiga SDK, NO real libc, NO libm.  Everything is hand-written:
 *   - entry/exit + the integer PutChar shim live in crt0.s (the same [J3] LVO bridge
 *     the whole corpus uses: jsr -30(a6));
 *   - the THREE transcendentals the C calls (sqrt/sin/exp) are tiny asm shims in crt0.s
 *     that execute the actual 68881 HARDWARE transcendental opcodes (FSQRT/FSIN/FETOX)
 *     on the fp0 argument — so the compiler's `jsr _sqrt` lands on a real line-F op.
 *
 * OUTPUT IS INTEGER-IZED.  We deliberately DON'T need the deferred FP->decimal `.p`
 * packed-decimal format: every FP result is scaled (x1000, or x1000000 for the small
 * variance) and converted to a 32-bit integer with the hardware double->int opcode
 * (the compiler lowers (long)d to fintrz.x + fmove.l fp0,d0), then printed through the
 * existing integer PutChar path (puti/putu).  So the program's OUTPUT is a stream of
 * scaled-integer decimals, fully observable, while the FP work itself is all hardware FP.
 *
 * WHAT FP IT EXERCISES (the compiler lowers each to the full m68k FP convention —
 * fmovem.x FP prologue/epilogue around the helpers, the AmigaOS stack-passed double
 * args, fp0 returns):
 *   (1) NEWTON'S METHOD for sqrt(x): the iteration g = (g + x/g)/2 — FP add/div/mul and
 *       an FP comparison |g*g - x| < eps each step (-> fcmp.d + fbge/fblt), looping until
 *       converged.  Compared against the HARDWARE fsqrt (the _sqrt shim) — the two must
 *       agree to the scaled integer.
 *   (2) a TAYLOR/MACLAURIN series sum for exp(x) = sum x^n/n! — a running FP product and
 *       sum, term-magnitude FP compare to stop (-> fcmp + fbcc).  Compared against the
 *       HARDWARE fetox (the _exp shim).
 *   (3) VECTOR STATISTICS over a double[] — mean, variance, standard deviation — with the
 *       stddev taken via the HARDWARE fsqrt.  The mean/variance helpers take/return
 *       doubles (fmovem.x FP register save/restore prologues, double args, fp0 return).
 *   (4) a small table of sin(theta) at theta = 0, pi/6, pi/4, pi/3, pi/2 via the HARDWARE
 *       fsin shim — exercises FP constants, the loop, and the transcendental.
 *
 * The independent interpreter (j5d_interp.c) models every one of these opcodes; the JIT
 * runs them through Emu68's REAL EMIT_FPU decoder + our FP shim.  PASS == byte-exact on
 * the FP register file (FP0..FP7 binary64 bits) + the integer regs + the WHOLE sandbox
 * memory + the full PutChar output stream + exit d0, with a negative control.
 *
 * PRECISION MODEL (mirrors Emu68 + the [J5o] spec): the FP register file is modeled in
 * IEEE-754 binary64; the JIT and the oracle run the SAME native double arithmetic, so the
 * results are deterministic and bit-exact between them (that is the gate).  80-bit extended
 * exactness is not bit-reproducible on AArch64 and is out of scope, exactly as documented.
 */

/* ---- the OS service + the hardware-FP shims (all in crt0.s) -------------------- */
extern void   putch(int c);     /* exec PutChar LVO -30 via jsr -30(a6) ([J3] bridge) */
extern double sqrt(double x);   /* crt0 shim: fmove.d 4(a7),fp0 ; fsqrt.x fp0 ; rts   */
extern double sin (double x);   /* crt0 shim: fmove.d 4(a7),fp0 ; fsin.x  fp0 ; rts   */
extern double exp (double x);   /* crt0 shim: fmove.d 4(a7),fp0 ; fetox.x fp0 ; rts   */

/* ---- tiny integer output helpers (no libc) — the SAME path j5m.c uses ---------- */
static void puts_(const char *s) { while (*s) putch((int)(unsigned char)*s++); }

static void putu(unsigned long v)            /* unsigned decimal */
{
    char buf[16]; int n = 0;
    if (v == 0) { putch('0'); return; }
    while (v) { buf[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) putch(buf[--n]);
}
static void puti(long v)                      /* signed decimal */
{
    if (v < 0) { putch('-'); putu((unsigned long)(-v)); }
    else        putu((unsigned long)v);
}
static void nl(void) { putch('\n'); }

/* ---- FP->scaled-integer (the integer-ization that avoids the deferred .p format) -
 * (long)(d * scale + (d<0?-0.5:0.5)) : the compiler lowers the cast to fintrz.x +
 * fmove.l fp0,d0 (the HARDWARE double->int op), so even the output marshalling is FP. */
static long scaled(double d, double scale)
{
    double v = d * scale;
    if (v < 0.0) return (long)(v - 0.5);
    return (long)(v + 0.5);
}
/* print a double as a fixed-point decimal with `places` fractional digits (places is the
 * log10 of the scale): integer part, '.', then the zero-padded fractional part. */
static void putfix(double d, long scale, int places)
{
    long s = scaled(d, (double)scale);
    if (s < 0) { putch('-'); s = -s; }
    long ip = s / scale, fp = s % scale;
    putu((unsigned long)ip);
    putch('.');
    /* zero-pad the fractional part to `places` digits */
    long p = scale / 10;
    while (p > fp && p > 0) { putch('0'); p /= 10; }
    if (fp == 0) { for (int i = 1; i < places; i++) putch('0'); }
    else putu((unsigned long)fp);
}

/* =================== (1) Newton's method for sqrt(x) ============================= */
static double newton_sqrt(double x)
{
    if (x <= 0.0) return 0.0;                 /* fcmp.d #0,x ; fble  */
    double g = x, prev = 0.0;
    int i;
    for (i = 0; i < 60; i++) {
        prev = g;
        g = (g + x / g) * 0.5;                /* fdiv + fadd + fmul  */
        double diff = g - prev;               /* fsub                */
        if (diff < 0.0) diff = -diff;         /* fcmp + fbge / fneg  */
        if (diff < 1e-12) break;              /* fcmp.d eps ; fblt   */
    }
    return g;
}

/* =================== (2) Taylor series for exp(x) =============================== */
static double taylor_exp(double x)
{
    double term = 1.0, sum = 1.0;
    int n;
    for (n = 1; n < 40; n++) {
        term = term * x / (double)n;          /* fmul + fmove.l(n)->FP + fdiv */
        sum = sum + term;                     /* fadd */
        double mag = term < 0.0 ? -term : term;
        if (mag < 1e-15) break;               /* fcmp + fblt */
    }
    return sum;
}

/* =================== (3) vector statistics over a double[] ====================== */
static double vec_mean(const double *a, int n)   /* FP helper: fmovem.x prologue */
{
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) s = s + a[i];
    return s / (double)n;
}
static double vec_variance(const double *a, int n, double mean)
{
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double d = a[i] - mean;
        s = s + d * d;                        /* fsub + fmul + fadd */
    }
    return s / (double)n;
}

int main(void)
{
    unsigned long checksum = 0;

    /* ---- (1) Newton's-method sqrt vs the HARDWARE fsqrt shim ------------------- */
    puts_("sqrt(Newton vs hw):\n");
    {
        double xs[5]; xs[0] = 2.0; xs[1] = 3.0; xs[2] = 10.0; xs[3] = 0.25; xs[4] = 1000.0;
        int i;
        for (i = 0; i < 5; i++) {
            double n = newton_sqrt(xs[i]);
            double h = sqrt(xs[i]);           /* jsr _sqrt -> FSQRT */
            puts_("  x="); putfix(xs[i], 1000, 3);
            puts_("  newton="); putfix(n, 1000000, 6);
            puts_("  hw=");     putfix(h, 1000000, 6);
            /* they must agree to 1e-6 (newton converged) -> fold the AGREEMENT in */
            double e = n - h; if (e < 0.0) e = -e;
            if (e < 1e-6) { puts_("  [agree]"); checksum += 1u; }
            else            puts_("  [DIVERGE]");
            nl();
            checksum = checksum * 31u + (unsigned long)scaled(n, 1000000.0);
        }
    }

    /* ---- (2) Taylor exp vs the HARDWARE fetox shim ---------------------------- */
    puts_("exp(Taylor vs hw):\n");
    {
        double xs[4]; xs[0] = 0.0; xs[1] = 1.0; xs[2] = 2.0; xs[3] = -1.5;
        int i;
        for (i = 0; i < 4; i++) {
            double t = taylor_exp(xs[i]);
            double h = exp(xs[i]);            /* jsr _exp -> FETOX */
            puts_("  x="); putfix(xs[i], 1000, 3);
            puts_("  taylor="); putfix(t, 1000000, 6);
            puts_("  hw=");     putfix(h, 1000000, 6);
            double e = t - h; if (e < 0.0) e = -e;
            if (e < 1e-6) { puts_("  [agree]"); checksum += 1u; }
            else            puts_("  [DIVERGE]");
            nl();
            checksum = checksum * 31u + (unsigned long)scaled(t, 1000.0);
        }
    }

    /* ---- (3) vector statistics: mean / variance / stddev ---------------------- */
    puts_("stats:\n");
    {
        double data[8];
        data[0] = 4.0;  data[1] = 8.0;  data[2] = 15.0; data[3] = 16.0;
        data[4] = 23.0; data[5] = 42.0; data[6] = 11.0; data[7] = 7.0;
        double mean = vec_mean(data, 8);
        double var  = vec_variance(data, 8, mean);
        double sd   = sqrt(var);              /* stddev via HARDWARE FSQRT */
        puts_("  mean=");   putfix(mean, 1000, 3); nl();
        puts_("  var=");    putfix(var,  1000, 3); nl();
        puts_("  stddev="); putfix(sd,   1000000, 6); nl();
        checksum = checksum * 31u + (unsigned long)scaled(mean, 1000.0);
        checksum = checksum * 31u + (unsigned long)scaled(var,  1000.0);
        checksum = checksum * 31u + (unsigned long)scaled(sd,   1000000.0);
        /* the variance must be non-negative and the stddev^2 must recover the variance */
        double back = sd * sd, e = back - var; if (e < 0.0) e = -e;
        if (e < 1e-6) checksum += 1u;         /* fcmp -> fblt */
    }

    /* ---- (4) a sin() table via the HARDWARE fsin shim ------------------------- */
    puts_("sin table:\n");
    {
        /* theta = 0, pi/6, pi/4, pi/3, pi/2 (radians) */
        double th[5];
        th[0] = 0.0;
        th[1] = 0.5235987755982988;   /* pi/6 */
        th[2] = 0.7853981633974483;   /* pi/4 */
        th[3] = 1.0471975511965976;   /* pi/3 */
        th[4] = 1.5707963267948966;   /* pi/2 */
        int i;
        for (i = 0; i < 5; i++) {
            double s = sin(th[i]);            /* jsr _sin -> FSIN */
            puts_("  sin("); putfix(th[i], 1000, 3); puts_(")=");
            putfix(s, 1000000, 6); nl();
            checksum = checksum * 31u + (unsigned long)scaled(s, 1000000.0);
        }
    }

    puts_("checksum=");
    putu(checksum);
    nl();

    /* return the low 16 bits of the checksum as the exit code (d0) — one value the JIT
     * run and the interpreter must agree on, on top of the byte-exact FP/reg/mem/output. */
    return (int)(checksum & 0xffffu);
}
