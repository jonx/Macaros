/* j3_test.c — [J3]: the 68k -> native LVO-call bridge, value-asserting (OURS).
 *
 * Clean-room / OURS. Standalone spike for docs/features/68k-jit/spec.md [J3]. Uses
 * no Emu68 source directly (the marshaller in j3_marshal.c does, behind the ABI).
 *
 * It proves the integration boundary in three grounded parts (no-crash is
 * necessary, never sufficient — a silent mis-marshal must NOT pass):
 *
 *  (1) VECTOR RECOGNITION (the dispatch math). For several LVO indices n, compute
 *      the 68k jump-target  libbase - n*6  via the REAL negative-offset rule
 *      (__AROS_GETJUMPVEC, LIB_VECTSIZE==6; arch/m68k-all/include/aros/cpu.h:82,81)
 *      and assert j3_vector_recognise round-trips it back to n. Negative controls:
 *      a PC above the base and a mis-aligned PC are both rejected.
 *
 *  (2) THE MARSHALLER. Three native stubs with DIFFERENT register maps, each
 *      declared with the REAL register-arg macros (AROS_LHA / AROS_UFHA). A per-LVO
 *      descriptor lists the ordered SOURCE 68k registers, taken directly from those
 *      declarations. The marshaller (EMITTED via the adopted Emu68 emitter into a
 *      MAP_JIT jit_region — see j3_marshal.c) reads those 68k registers from a
 *      struct M68KState, places them in AAPCS64 x0..x7, blr's the stub, and stores
 *      the native return into 68k d0.
 *
 *  (3) VERIFY. Each stub RECORDS the exact argument values it received. For each
 *      function we assert (a) the stub saw the 68k register values, marshalled into
 *      the right AAPCS64 slots, and (b) 68k d0 received the stub's return. PASS only
 *      if every function's args AND return are exact.
 *
 * GROUNDING of each descriptor against the real AROS register-map macros is stated
 * at each stub (AROS_LHA = libcall.h:1586 -> type,name,reg ; AROS_UFHA =
 * asmcall.h:822 ; reg D0..A7 -> physical %d0..%a7 via cpu.h:125-139 + gencall.c:309).
 *
 * Watchdog: a SIGALRM hard-kills the process so the spike can never hang.
 */
#include "j3_jit68k.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

/* ---- We define AROS_LHA / AROS_UFHA exactly as the real AROS headers do, so the
 * register maps below are written in the genuine AROS notation (the triplet
 * (type,name,reg)). Source of truth:
 *   compiler/arossupport/include/libcall.h:1586  #define AROS_LHA(t,n,r) t,n,r
 *   compiler/arossupport/include/asmcall.h:822    #define AROS_UFHA(t,n,r) t,n,r
 * (We can't include those headers standalone — they pull the whole AROS system —
 * so we restate the two one-line definitions verbatim. The `reg` token is the 68k
 * source register that the call machinery loads each argument into, cpu.h:125-139,
 * gencall.c:309.) These are used purely as documentation of the contract; the
 * actual descriptor uses the j3_m68k_reg enum that mirrors the same registers. */
#ifndef AROS_LHA
#define AROS_LHA(type,name,reg)  type,name,reg
#endif
#ifndef AROS_UFHA
#define AROS_UFHA(type,name,reg) type,name,reg
#endif

/* 68k register symbols (cpu.h: D0..D7, A0..A7) used only to write the maps below
 * in AROS notation. */
enum { D0,D1,D2,D3,D4,D5,D6,D7, A0,A1,A2,A3,A4,A5,A6,A7 };

/* ====================== Native AArch64 AROS_LH stub functions =================
 * Each records the EXACT args it received, so the test can assert the 68k register
 * values were marshalled into the right AAPCS64 slots. A 32-bit 68k register
 * arrives in the low 32 bits of its AAPCS64 x-register; the stub takes uint32_t. */

static struct {
    int      called;
    uint32_t a0, a1, a2;   /* up to 3 captured args (AAPCS64 order) */
} cap1, cap2, cap3;

/* --- Function 1: one DATA-register arg, returns a value into d0. --------------
 * AROS declaration shape (the register map we ground against):
 *   AROS_LH1(ULONG, Square,
 *            AROS_LHA(ULONG, n, D0),         <-- arg in 68k D0
 *            ... lvo ...)
 * Returns ULONG -> 68k d0 (gencall.c:153 sizeof(t)<QUAD => _ret0 asm("%d0")).
 * Marshal: x0 <- d[0]; blr; d0 <- w0. */
static uint32_t stub_square(uint32_t n)
{
    cap1.called = 1;
    cap1.a0 = n;
    return n * n;                /* a recognisable, value-asserted transform */
}

/* --- Function 2: a DATA arg + an ADDRESS arg (the classic mixed map). ---------
 * AROS declaration shape:
 *   AROS_LH2(ULONG, WriteN,
 *            AROS_LHA(ULONG, count, D0),     <-- arg0 in 68k D0
 *            AROS_LHA(APTR,  buf,   A0),     <-- arg1 in 68k A0
 *            ... lvo ...)
 * Mixed D/A in a single call is the case most likely to mis-marshal — exactly what
 * [J3] must guard. Returns count+1 so the return is also asserted. */
static uint32_t stub_writen(uint32_t count, uint32_t buf /*sandbox addr*/)
{
    cap2.called = 1;
    cap2.a0 = count;
    cap2.a1 = buf;
    return count + 1u;
}

/* --- Function 3: NON-first, OUT-OF-ORDER registers (A1, D1, D2), void return. -
 * AROS declaration shape (no return -> nothing written to d0):
 *   AROS_LH3(void, Blit,
 *            AROS_LHA(APTR,  src, A1),       <-- arg0 in 68k A1
 *            AROS_LHA(ULONG, len, D1),       <-- arg1 in 68k D1
 *            AROS_LHA(UWORD, mode,D2),       <-- arg2 in 68k D2
 *            ... lvo ...)
 * Proves the descriptor faithfully follows whatever registers (and order) the FD
 * declares, not a fixed D0/A0 convention. */
static void stub_blit(uint32_t src, uint32_t len, uint32_t mode)
{
    cap3.called = 1;
    cap3.a0 = src;
    cap3.a1 = len;
    cap3.a2 = mode;
}

/* ============================ watchdog ======================================= */
static void watchdog(int sig)
{
    (void)sig;
    const char *m = "[J3] FAIL: watchdog timeout (thunk hung or faulted)\n";
    write(2, m, strlen(m));
    _exit(2);
}

/* Seed a NONZERO architectural file so the asserts prove the bridge read the REAL
 * registers (not that a zeroed struct matched a zeroed struct). Each register gets
 * a recognisable sentinel; the per-function callers overwrite the specific source
 * registers with the values they want to see arrive at the stub. */
static void seed_state(struct M68KState *st)
{
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < 8; i++) st->d[i] = 0xD0D00000u + (uint32_t)i;
    for (int i = 0; i < 8; i++) st->a[i] = 0xA0A00000u + (uint32_t)i;
    st->ccr = 0x00000010u;
    st->pc  = 0x00021000u;
}

static int check(const char *label, uint32_t got, uint32_t want)
{
    int bad = (got != want);
    printf("      %-28s got=0x%08X want=0x%08X  %s\n",
           label, got, want, bad ? "<-- MISMATCH" : "ok");
    return bad;
}

/* =============================== part (1) ==================================== */
static int test_vector_recognition(void)
{
    printf("[J3] (1) vector recognition — the dispatch math (lib - n*6, __AROS_GETJUMPVEC)\n");
    const uint32_t base = 0x00080000u;     /* a fake 68k-space library base */
    int bad = 0;

    /* Round-trip several LVO indices through addr -> recognise. */
    const int ns[] = { 1, 2, 5, 42, 119 };
    for (unsigned i = 0; i < sizeof(ns)/sizeof(ns[0]); i++) {
        int n = ns[i];
        uint32_t va = j3_vector_addr(base, n);     /* base - n*6 */
        int rec = j3_vector_recognise(base, va);
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "n=%d @0x%08X -> recover", n, va);
        bad |= check(lbl, (uint32_t)rec, (uint32_t)n);
        /* also assert the address is exactly the negative-offset arithmetic */
        bad |= check("    addr == base-n*6", va, base - (uint32_t)n * 6u);
    }

    /* Negative controls: above the base, and a mis-aligned PC, must be rejected. */
    int above = j3_vector_recognise(base, base + 6);
    printf("      %-28s got=%d (want -1)  %s\n", "reject pc above base",
           above, (above == -1) ? "ok" : "<-- MISMATCH");
    bad |= (above != -1);

    int misal = j3_vector_recognise(base, base - 7);   /* 7 not a multiple of 6 */
    printf("      %-28s got=%d (want -1)  %s\n", "reject misaligned pc",
           misal, (misal == -1) ? "ok" : "<-- MISMATCH");
    bad |= (misal != -1);

    printf("[J3] (1) %s\n", bad ? "FAIL" : "PASS");
    return bad;
}

/* =============================== part (2)+(3) ================================ */
static int run_one(const j3_lvo_desc *desc,
                   void (*seed_args)(struct M68KState *),
                   int (*verify)(const struct M68KState *before,
                                 const struct M68KState *after))
{
    char err[128] = {0};
    j3_thunk_fn thunk = j3_build_marshal_thunk(desc, err, sizeof(err));
    if (!thunk) {
        printf("[J3]   FAIL: could not build thunk for %s: %s\n",
               desc->name, err[0] ? err : "(unknown)");
        return 1;
    }

    struct M68KState before, st;
    seed_state(&st);
    seed_args(&st);
    before = st;                       /* snapshot the seeded 68k registers */

    /* Recognise the call first (prove the dispatcher would route here), then run
     * the emitted marshal thunk under W^X. */
    uint32_t va = j3_vector_addr(desc->libbase, desc->lvo);
    int rec = j3_vector_recognise(desc->libbase, va);
    printf("[J3]   %s: LVO %d of base 0x%08X -> vector 0x%08X, recognised n=%d\n",
           desc->name, desc->lvo, desc->libbase, va, rec);

    thunk(&st);                        /* <-- executes freshly-emitted AArch64 */

    int bad = (rec != desc->lvo);
    bad |= verify(&before, &st);
    return bad;
}

/* ---- per-function arg seeders + verifiers ---- */

static void seed_square(struct M68KState *st) { st->d[0] = 9u; }            /* D0=9 */
static int verify_square(const struct M68KState *b, const struct M68KState *a)
{
    int bad = 0;
    printf("      stub_square: declared AROS_LHA(ULONG,n,D0) -> arg0 from D0\n");
    bad |= (cap1.called ? 0 : (printf("      stub NOT called\n"), 1));
    bad |= check("arg0 (x0) == D0", cap1.a0, b->d[0]);          /* saw 68k D0 */
    bad |= check("d0 == return (n*n)", a->d[0], b->d[0] * b->d[0]); /* return in d0 */
    return bad;
}

static void seed_writen(struct M68KState *st) { st->d[0] = 0x0000002Au; st->a[0] = 0x00012340u; }
static int verify_writen(const struct M68KState *b, const struct M68KState *a)
{
    int bad = 0;
    printf("      stub_writen: AROS_LHA(ULONG,count,D0)+AROS_LHA(APTR,buf,A0) -> x0=D0,x1=A0\n");
    bad |= (cap2.called ? 0 : (printf("      stub NOT called\n"), 1));
    bad |= check("arg0 (x0) == D0 (count)", cap2.a0, b->d[0]);
    bad |= check("arg1 (x1) == A0 (buf)",   cap2.a1, b->a[0]);
    bad |= check("d0 == return (count+1)",  a->d[0], b->d[0] + 1u);
    return bad;
}

static void seed_blit(struct M68KState *st) { st->a[1] = 0x00033000u; st->d[1] = 0x00000100u; st->d[2] = 0x00000007u; }
static int verify_blit(const struct M68KState *b, const struct M68KState *a)
{
    int bad = 0;
    printf("      stub_blit: AROS_LHA(APTR,src,A1)+AROS_LHA(ULONG,len,D1)+AROS_LHA(UWORD,mode,D2)\n");
    bad |= (cap3.called ? 0 : (printf("      stub NOT called\n"), 1));
    bad |= check("arg0 (x0) == A1 (src)",  cap3.a0, b->a[1]);
    bad |= check("arg1 (x1) == D1 (len)",  cap3.a1, b->d[1]);
    bad |= check("arg2 (x2) == D2 (mode)", cap3.a2, b->d[2]);
    /* void return: d0 must be UNCHANGED from its seeded sentinel. */
    bad |= check("d0 unchanged (void ret)", a->d[0], b->d[0]);
    return bad;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    alarm(10);

    printf("[J3] 68k -> native LVO-call bridge: vector recognition + marshaller + return\n");
    printf("[J3]   marshaller realization: EMITTED via the adopted Emu68 emitter into a\n");
    printf("[J3]   MAP_JIT jit_region (the reverse of the H3 host-call shim).\n");

    int bad = 0;

    /* (1) the dispatch math */
    bad |= test_vector_recognition();

    /* (2)+(3) three functions, different register maps, value-asserted */
    printf("[J3] (2)+(3) marshaller + return, three functions with different register maps\n");

    static const j3_lvo_desc d_square = {
        .name = "Square", .libbase = 0x00080000u, .lvo = 5,
        .nargs = 1, .src = { J3_D0 }, .returns = 1,
        .stub = (void(*)(void))stub_square,
    };
    static const j3_lvo_desc d_writen = {
        .name = "WriteN", .libbase = 0x00080000u, .lvo = 7,
        .nargs = 2, .src = { J3_D0, J3_A0 }, .returns = 1,
        .stub = (void(*)(void))stub_writen,
    };
    static const j3_lvo_desc d_blit = {
        .name = "Blit", .libbase = 0x00080000u, .lvo = 12,
        .nargs = 3, .src = { J3_A1, J3_D1, J3_D2 }, .returns = 0,
        .stub = (void(*)(void))stub_blit,
    };

    bad |= run_one(&d_square, seed_square, verify_square);
    bad |= run_one(&d_writen, seed_writen, verify_writen);
    bad |= run_one(&d_blit,   seed_blit,   verify_blit);

    j3_free_all_thunks();

    if (!bad) {
        printf("[J3] PASS: vector recognition round-trips (lib-n*6, real __AROS_GETJUMPVEC); "
               "all three stubs saw the exact 68k register values marshalled into the right "
               "AAPCS64 slots (D0; D0+A0; A1+D1+D2), and 68k d0 received each return. "
               "Reverse-H3 marshal thunk emitted by the adopted Emu68 emitter, run under MAP_JIT.\n");
    } else {
        printf("[J3] FAIL: a vector index, marshalled argument, or return was wrong — the "
               "register map is mismarshalled (see the per-line MISMATCH markers above).\n");
    }
    return bad ? 1 : 0;
}
