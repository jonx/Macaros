/* j5a_test.c — [J5a] translate a memory-touching block, verify registers AND sandbox
 * memory against an INDEPENDENT reference. (OURS, AROS-licensed; touches no Emu68
 * source directly.)
 *
 * Clean-room / OURS. Standalone spike for docs/features/68k-jit/spec.md [J5a].
 *
 * What it proves, unattended, value-asserting (no-crash is necessary, never
 * sufficient — a silent mistranslation or a missed byteswap must not pass):
 *
 *   The 68k block        move.l (a0),d0 ; addq #1,d0 ; move.l d0,(a0) ; rts
 *   (load a longword from the sandbox via A0, increment it, store it back) is loaded
 *   from a REAL big-endian AmigaOS hunk binary into a 32-bit sandbox (the [J4] loader,
 *   reused), then translated to AArch64 by the ADOPTED Emu68 emitter driven by our
 *   HAND-ROLLED EA + sandbox-pointer path (j5a_build.c), and RUN under W^X. A0 is
 *   seeded to point at a DATA slot holding a known big-endian longword.
 *
 *   VALUE ASSERTS (PASS iff ALL hold):
 *     (a) the JITed register file (d0..d7/a0..a7) == the INDEPENDENT interpreter's
 *         (j5a_interp.c), field by field;
 *     (b) the JITed sandbox MEMORY (the longword at A0) == the interpreter's sandbox,
 *         byte-exact big-endian — proving the store landed and the byteswap is right;
 *     (c) d0 == initial_value + 1 and the stored longword == initial_value + 1.
 *
 *   NEGATIVE CONTROLS (each must make the asserts BITE):
 *     1. skip-store: emit the block WITHOUT the `move.l d0,(a0)` — the sandbox memory
 *        then stays at the initial value, diverging from the interpreter.
 *     2. wrong-endianness: emit the load/store WITHOUT the REV byteswap — the JITed
 *        result diverges from the big-endian interpreter.
 *     3. out-of-range: seed A0 outside the sandbox — the block must fault CLEANLY
 *        (state->fault set, no host out-of-bounds access, no crash).
 *
 * Watchdog: a SIGALRM hard-kills the process so the spike can never hang.
 */
#include "j4_hunk.h"        /* reuse the [J4] loader/relocator + sandbox (shared) */
#include "j5a_jit68k.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static void watchdog(int sig)
{
    (void)sig;
    const char *m = "[J5a] FAIL: watchdog timeout (translate/run hung or faulted)\n";
    write(2, m, strlen(m));
    _exit(2);
}

/* ---- Hand-assemble the REAL hunk binary, big-endian ---------------------------
 * Two hunks. Hunk 0 CODE holds the entry block; hunk 1 DATA holds the longword the
 * block loads/stores. A HUNK_RELOC32 is NOT strictly needed for the memory op (we
 * seed A0 at run time), but we keep the loader path identical to [J4].
 *
 *   move.l (a0),d0  = 0x2010   (0010 ddd=000 mode=000 / src 010 reg=000 -> (A0))
 *   addq.l #1,d0    = 0x5280   (0101 q=001 0 sz=10 mode=000 reg=000 -> #1,D0)
 *   move.l d0,(a0)  = 0x2080   (0010 dst-reg=000 mode=010 / src 000 reg=000 -> D0)
 *   rts             = 0x4E75
 * Code is 4 words = 8 bytes = 2 longwords.
 */
#define ENTRY_W0  0x2010u    /* move.l (a0),d0 */
#define ENTRY_W1  0x5280u    /* addq.l #1,d0   */
#define ENTRY_W2  0x2080u    /* move.l d0,(a0) */
#define ENTRY_W3  0x4E75u    /* rts            */

#define INIT_VALUE  0x12345678u    /* the longword in the DATA slot (big-endian) */
#define EXPECT_D0   (INIT_VALUE + 1u)

static void put_be32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v >> 24); (*p)[1] = (uint8_t)(v >> 16);
    (*p)[2] = (uint8_t)(v >>  8); (*p)[3] = (uint8_t)(v      );
    *p += 4;
}

static size_t build_hunk_binary(uint8_t *out)
{
    uint8_t *p = out;
    /* HUNK_HEADER */
    put_be32(&p, J4_HUNK_HEADER);
    put_be32(&p, 0);            /* empty name list */
    put_be32(&p, 2);            /* numhunks = 2 (0=CODE, 1=DATA) */
    put_be32(&p, 0);            /* first */
    put_be32(&p, 1);            /* last  */
    put_be32(&p, 2);            /* hunk0 size = 2 longs (4 opcode words) */
    put_be32(&p, 1);            /* hunk1 size = 1 long  (the data slot)  */
    /* HUNK_CODE (hunk 0): the entry block, 2 longwords */
    put_be32(&p, J4_HUNK_CODE);
    put_be32(&p, 2);            /* length = 2 longs */
    put_be32(&p, (ENTRY_W0 << 16) | ENTRY_W1);   /* 0x2010 0x5280 */
    put_be32(&p, (ENTRY_W2 << 16) | ENTRY_W3);   /* 0x2080 0x4E75 */
    /* HUNK_DATA (hunk 1): the longword the block loads/stores */
    put_be32(&p, J4_HUNK_DATA);
    put_be32(&p, 1);            /* length = 1 long */
    put_be32(&p, INIT_VALUE);   /* big-endian initial value */
    /* HUNK_END */
    put_be32(&p, J4_HUNK_END);
    return (size_t)(p - out);
}

/* Read a 32-bit big-endian value from a sandbox host pointer. */
static uint32_t sandbox_be32(const j4_sandbox *sb, uint32_t addr)
{
    const uint8_t *p = j4_sandbox_host(sb, addr);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x00010000u     /* 64 KiB */

/* Translate a j4_sandbox into the j5a_sandbox view (same memory, same origin). */
static j5a_sandbox as_j5a(const j4_sandbox *sb)
{
    j5a_sandbox s = { sb->host_mem, sb->sandbox_origin, sb->size };
    return s;
}

/* Compare two register files; print the first mismatch. Returns 1 if equal. */
static int regs_equal(const struct j5a_m68k_state *a, const struct j5a_m68k_state *b)
{
    int ok = 1;
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) {
        printf("[J5a]     D%d mismatch: jit=0x%08X ref=0x%08X\n", i, a->d[i], b->d[i]); ok = 0;
    }
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) {
        printf("[J5a]     A%d mismatch: jit=0x%08X ref=0x%08X\n", i, a->a[i], b->a[i]); ok = 0;
    }
    if (a->fault != b->fault) {
        printf("[J5a]     fault mismatch: jit=0x%08X ref=0x%08X\n", a->fault, b->fault); ok = 0;
    }
    return ok;
}

/* Load+relocate the binary into a fresh sandbox over `host_mem`. Returns 0 on ok. */
static int load_fresh(uint8_t *host_mem, const uint8_t *bin, size_t binlen,
                      j4_sandbox *sb, j4_seglist *seg, char *err, unsigned errlen)
{
    j4_sandbox_init(sb, host_mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    return j4_load_hunks(sb, bin, binlen, /*skip_reloc=*/0, seg, err, errlen);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    alarm(10);

    printf("[J5a] translate a memory-touching block + the sandbox-pointer boundary, verify vs an independent reference\n");
    printf("[J5a]   block: move.l (a0),d0 ; addq #1,d0 ; move.l d0,(a0) ; rts   (load via A0, +1, store back)\n");

    uint8_t bin[256];
    size_t  binlen = build_hunk_binary(bin);
    char    err[200] = {0};

    /* Each run gets its OWN host memory so the JIT and the interpreter operate on
     * independent copies of the identical initial sandbox (no cross-contamination). */
    uint8_t *mem_jit = malloc(SANDBOX_SIZE);
    uint8_t *mem_ref = malloc(SANDBOX_SIZE);
    if (!mem_jit || !mem_ref) { printf("[J5a] FAIL: malloc sandbox failed\n"); return 1; }

    /* ---------- LOAD the binary into both sandboxes ---------- */
    j4_sandbox sbj, sbr; j4_seglist segj, segr;
    if (load_fresh(mem_jit, bin, binlen, &sbj, &segj, err, sizeof(err)) ||
        load_fresh(mem_ref, bin, binlen, &sbr, &segr, err, sizeof(err))) {
        printf("[J5a] FAIL: load error: %s\n", err);
        return 1;
    }
    uint32_t code_base = segj.hunk_base[0];
    uint32_t data_base = segj.hunk_base[1];      /* A0 will point here */
    const uint8_t *code = j4_sandbox_host(&sbj, segj.entry);
    uint32_t code_len = segj.hunk_size[0];
    printf("[J5a]   loaded: CODE @ 0x%08X (%u B), DATA @ 0x%08X holding BE 0x%08X; A0:=DATA\n",
           code_base, code_len, data_base, INIT_VALUE);

    /* ================= MAIN CASE ================= */
    /* Seed identical states: A0 -> the DATA slot, everything else zero. */
    struct j5a_m68k_state st_jit, st_ref;
    memset(&st_jit, 0, sizeof(st_jit)); st_jit.a[0] = data_base;
    memset(&st_ref, 0, sizeof(st_ref)); st_ref.a[0] = data_base;

    j5a_sandbox vj = as_j5a(&sbj), vr = as_j5a(&sbr);

    /* JIT path. */
    if (j5a_run_block(&vj, segj.entry, code, code_len, &st_jit,
                      /*neg_endianness=*/0, /*neg_skip_store=*/0, err, sizeof(err)) != 0) {
        printf("[J5a] FAIL: run error: %s\n", err);
        return 1;
    }
    j5a_run_free();

    /* INDEPENDENT reference. */
    j5a_interp_run_block(&vr, segr.entry, j4_sandbox_host(&sbr, segr.entry), code_len, &st_ref);

    int regs_ok = regs_equal(&st_jit, &st_ref);
    uint32_t mem_jit_val = sandbox_be32(&sbj, data_base);
    uint32_t mem_ref_val = sandbox_be32(&sbr, data_base);
    int mem_ok = (mem_jit_val == mem_ref_val);
    int val_ok = (st_jit.d[0] == EXPECT_D0) && (mem_jit_val == EXPECT_D0) && (st_jit.fault == 0);

    printf("[J5a]   JIT: d0=0x%08X a0=0x%08X fault=0x%08X ; sandbox[A0]=BE 0x%08X\n",
           st_jit.d[0], st_jit.a[0], st_jit.fault, mem_jit_val);
    printf("[J5a]   REF: d0=0x%08X a0=0x%08X fault=0x%08X ; sandbox[A0]=BE 0x%08X\n",
           st_ref.d[0], st_ref.a[0], st_ref.fault, mem_ref_val);
    printf("[J5a]   ASSERT registers JIT==REF -> %s ; sandbox-memory JIT==REF (byte-exact BE) -> %s ; "
           "d0==value+1 and stored==value+1 -> %s\n",
           regs_ok ? "MATCH" : "MISMATCH", mem_ok ? "MATCH" : "MISMATCH", val_ok ? "MATCH" : "MISMATCH");

    /* ================= NEGATIVE CONTROL 1: skip the store ================= */
    j4_sandbox sbc; j4_seglist segc; uint8_t *mem_c = malloc(SANDBOX_SIZE);
    int ctl_skip_ok = 0;
    if (mem_c && load_fresh(mem_c, bin, binlen, &sbc, &segc, err, sizeof(err)) == 0) {
        struct j5a_m68k_state stc; memset(&stc, 0, sizeof(stc)); stc.a[0] = segc.hunk_base[1];
        j5a_sandbox vc = as_j5a(&sbc);
        if (j5a_run_block(&vc, segc.entry, j4_sandbox_host(&sbc, segc.entry),
                          segc.hunk_size[0], &stc, 0, /*neg_skip_store=*/1, err, sizeof(err)) == 0) {
            j5a_run_free();
            uint32_t v = sandbox_be32(&sbc, segc.hunk_base[1]);
            /* store skipped -> memory unchanged (still INIT_VALUE), differs from EXPECT. */
            ctl_skip_ok = (v == INIT_VALUE) && (v != EXPECT_D0);
            printf("[J5a]   NEG CONTROL 1 (skip store): sandbox[A0]=BE 0x%08X (unchanged) -> %s\n",
                   v, ctl_skip_ok ? "correctly WRONG (memory assert bites)" : "unexpected");
        }
    }
    free(mem_c);

    /* ================= NEGATIVE CONTROL 2: wrong endianness ================= */
    j4_sandbox sbe; j4_seglist sege; uint8_t *mem_e = malloc(SANDBOX_SIZE);
    int ctl_endian_ok = 0;
    if (mem_e && load_fresh(mem_e, bin, binlen, &sbe, &sege, err, sizeof(err)) == 0) {
        struct j5a_m68k_state ste; memset(&ste, 0, sizeof(ste)); ste.a[0] = sege.hunk_base[1];
        j5a_sandbox ve = as_j5a(&sbe);
        if (j5a_run_block(&ve, sege.entry, j4_sandbox_host(&sbe, sege.entry),
                          sege.hunk_size[0], &ste, /*neg_endianness=*/1, 0, err, sizeof(err)) == 0) {
            j5a_run_free();
            /* Without REV, the load reads the bytes little-endian; d0 must differ from
             * the big-endian reference's EXPECT_D0. */
            ctl_endian_ok = (ste.d[0] != EXPECT_D0);
            printf("[J5a]   NEG CONTROL 2 (no byteswap): d0=0x%08X (expected != 0x%08X) -> %s\n",
                   ste.d[0], EXPECT_D0, ctl_endian_ok ? "correctly WRONG (endianness assert bites)" : "unexpected");
        }
    }
    free(mem_e);

    /* ================= NEGATIVE CONTROL 3: out-of-range A0 faults cleanly ========= */
    j4_sandbox sbo; j4_seglist sego; uint8_t *mem_o = malloc(SANDBOX_SIZE);
    int ctl_oob_ok = 0;
    if (mem_o && load_fresh(mem_o, bin, binlen, &sbo, &sego, err, sizeof(err)) == 0) {
        struct j5a_m68k_state sto; memset(&sto, 0, sizeof(sto));
        sto.a[0] = SANDBOX_ORIGIN + SANDBOX_SIZE + 0x1000u;   /* well past the sandbox end */
        j5a_sandbox vo = as_j5a(&sbo);
        if (j5a_run_block(&vo, sego.entry, j4_sandbox_host(&sbo, sego.entry),
                          sego.hunk_size[0], &sto, 0, 0, err, sizeof(err)) == 0) {
            j5a_run_free();
            /* The block must have faulted on the load (no host OOB) — fault flag set,
             * and no crash got us here. */
            ctl_oob_ok = (sto.fault == J5A_FAULT_OOB);
            printf("[J5a]   NEG CONTROL 3 (A0 out of range 0x%08X): fault=0x%08X -> %s\n",
                   sto.a[0], sto.fault, ctl_oob_ok ? "faulted CLEANLY (no host OOB)" : "did NOT fault");
        }
    }
    free(mem_o);

    free(mem_jit); free(mem_ref);

    int ok = regs_ok && mem_ok && val_ok && ctl_skip_ok && ctl_endian_ok && ctl_oob_ok;
    if (ok) {
        printf("[J5a] PASS: memory load/store via the sandbox-pointer boundary translated by the adopted Emu68 "
               "emitter (hand-rolled EA path) is byte-exact vs an independent reference — registers AND sandbox "
               "memory match, big-endian; store landed (d0=0x%08X, sandbox[A0]=0x%08X); skip-store, wrong-endianness, "
               "and out-of-range negative controls all bite.\n", st_jit.d[0], mem_jit_val);
    } else {
        printf("[J5a] FAIL: %s%s%s%s%s%s\n",
               regs_ok       ? "" : "register mismatch ",
               mem_ok        ? "" : "sandbox-memory mismatch ",
               val_ok        ? "" : "value/fault assert ",
               ctl_skip_ok   ? "" : "skip-store control did not bite ",
               ctl_endian_ok ? "" : "endianness control did not bite ",
               ctl_oob_ok    ? "" : "out-of-range control did not fault ");
    }
    return ok ? 0 : 1;
}
