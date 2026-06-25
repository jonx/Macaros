/* j4_test.c — [J4] real hunk binary, end to end: load -> relocate -> place in
 * sandbox -> translate -> run -> return. (OURS, AROS-licensed; touches no Emu68
 * source directly.)
 *
 * Clean-room / OURS. Standalone spike for docs/features/68k-jit/spec.md [J4].
 *
 * What it proves, unattended, value-asserting (no-crash is necessary, never
 * sufficient — a silent mistranslation/misrelocation must not pass):
 *
 *   1. A tiny REAL big-endian AmigaOS hunk binary (hand-assembled below) — a
 *      HUNK_CODE (`moveq #42,d0 ; rts`), a HUNK_DATA (one 32-bit pointer slot with a
 *      nonzero addend), and a HUNK_RELOC32 that patches that DATA slot to point into
 *      the CODE hunk — is parsed by our minimal hunk loader and placed into a 32-bit
 *      sandbox, with HUNK_RELOC32 applied EXACTLY as rom/dos/internalloadseg_aos.c
 *      does (read BE32 at offset, add the target hunk's sandbox base, write BE32).
 *   2. The entry (first CODE hunk) is translated through the [J2] Emu68-emitter path
 *      in a MAP_JIT region and RUN under W^X, returning the 68k d0.
 *
 *   VALUE ASSERTS (PASS iff BOTH hold):
 *     (a) the relocated DATA pointer, read back big-endian from the sandbox, equals
 *         the EXPECTED sandbox runtime address (CODE base + the addend) — byte-exact,
 *         big-endian. Relocation is genuinely exercised, not a no-op.
 *     (b) the executed entry returns d0 == 42 (the program's chosen exit code).
 *
 *   NEGATIVE CONTROL: the same binary is loaded a second time with relocation
 *   SKIPPED; the DATA pointer then holds only the raw addend (wrong), proving the
 *   relocation assert in (a) actually bites.
 *
 * Watchdog: a SIGALRM hard-kills the process so the spike can never hang.
 */
#include "j4_hunk.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static void watchdog(int sig)
{
    (void)sig;
    const char *m = "[J4] FAIL: watchdog timeout (load/relocate/run hung or faulted)\n";
    write(2, m, strlen(m));
    _exit(2);
}

/* ---- Hand-assemble the REAL hunk binary, big-endian ---------------------------
 * Layout (every longword is 32-bit BIG-ENDIAN, the AmigaOS executable format):
 *
 *   HUNK_HEADER (1011)
 *     0                      ; end of (empty) library-name list
 *     2                      ; numhunks = 2  (hunk 0 = CODE, hunk 1 = DATA)
 *     0                      ; first = 0
 *     1                      ; last  = 1
 *     1                      ; hunk 0 size = 1 longword (the code: moveq+rts)
 *     1                      ; hunk 1 size = 1 longword (the pointer slot)
 *
 *   HUNK_CODE (1001)         ; hunk 0
 *     1                      ; length = 1 longword
 *     0x702A4E75             ; moveq #42,d0 (0x702A) ; rts (0x4E75)  -- entry, d0=42
 *
 *   HUNK_DATA (1002)         ; hunk 1
 *     1                      ; length = 1 longword
 *     0x00000008             ; pointer slot: addend 8 (nonzero, to prove val=orig+base)
 *
 *   HUNK_RELOC32 (1004)      ; relocations for the LAST-loaded hunk (= DATA, hunk 1)
 *     1                      ; count = 1 offset
 *     0                      ; target hunk = 0 (CODE) -> add CODE's runtime base
 *     0                      ; offset = 0 within DATA
 *     0                      ; end-of-table marker
 *
 *   HUNK_END (1010)
 *
 * After relocation the DATA slot must read (big-endian) = CODE_base + 8.
 */
#define DATA_ADDEND  0x00000008u
#define EXPECT_D0    42u

static void put_be32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v >> 24);
    (*p)[1] = (uint8_t)(v >> 16);
    (*p)[2] = (uint8_t)(v >>  8);
    (*p)[3] = (uint8_t)(v      );
    *p += 4;
}

static size_t build_hunk_binary(uint8_t *out)
{
    uint8_t *p = out;
    /* HUNK_HEADER */
    put_be32(&p, J4_HUNK_HEADER);
    put_be32(&p, 0);            /* empty name list */
    put_be32(&p, 2);            /* numhunks */
    put_be32(&p, 0);            /* first */
    put_be32(&p, 1);            /* last */
    put_be32(&p, 1);            /* hunk0 size = 1 long */
    put_be32(&p, 1);            /* hunk1 size = 1 long */
    /* HUNK_CODE (hunk 0): moveq #42,d0 ; rts */
    put_be32(&p, J4_HUNK_CODE);
    put_be32(&p, 1);            /* length = 1 long */
    put_be32(&p, 0x702A4E75u);  /* 0x702A=moveq #42,d0 ; 0x4E75=rts */
    /* HUNK_DATA (hunk 1): one pointer slot holding the addend */
    put_be32(&p, J4_HUNK_DATA);
    put_be32(&p, 1);            /* length = 1 long */
    put_be32(&p, DATA_ADDEND);  /* addend (nonzero) */
    /* HUNK_RELOC32 (patches DATA, the last-loaded hunk) */
    put_be32(&p, J4_HUNK_RELOC32);
    put_be32(&p, 1);            /* count = 1 */
    put_be32(&p, 0);            /* target hunk = 0 (CODE) */
    put_be32(&p, 0);            /* offset = 0 */
    put_be32(&p, 0);            /* end of reloc table */
    /* HUNK_END */
    put_be32(&p, J4_HUNK_END);

    return (size_t)(p - out);
}

/* Read a 32-bit big-endian value from a sandbox host pointer (what a relocated
 * pointer must equal, byte-exact). */
static uint32_t sandbox_be32(const j4_sandbox *sb, uint32_t addr)
{
    const uint8_t *p = j4_sandbox_host(sb, addr);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* A 68k-space sandbox origin chosen nonzero (and 32-bit) so a relocated pointer is
 * a real runtime base, not host pointer 0. */
#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x00010000u     /* 64 KiB */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    alarm(10);

    printf("[J4] real hunk binary, end to end: load -> relocate -> sandbox -> translate -> run -> return\n");
    printf("[J4]   hunk: HUNK_CODE(moveq #%u,d0 ; rts) + HUNK_DATA(ptr=0x%X) + HUNK_RELOC32(DATA[0] += CODE_base)\n",
           EXPECT_D0, DATA_ADDEND);

    /* The 32-bit sandbox: a host malloc'd region; 68k addr `a` -> host_mem[a-origin]. */
    uint8_t *host_mem = malloc(SANDBOX_SIZE);
    if (!host_mem) { printf("[J4] FAIL: malloc sandbox failed\n"); return 1; }

    uint8_t bin[256];
    size_t  binlen = build_hunk_binary(bin);
    printf("[J4]   assembled %zu-byte big-endian hunk file\n", binlen);

    char err[160] = {0};

    /* ---------- (1) LOAD + RELOCATE (the real path) ---------- */
    j4_sandbox sb;
    j4_sandbox_init(&sb, host_mem, SANDBOX_ORIGIN, SANDBOX_SIZE);

    j4_seglist seg;
    if (j4_load_hunks(&sb, bin, binlen, /*skip_reloc=*/0, &seg, err, sizeof(err)) != 0) {
        printf("[J4] FAIL: load/relocate error: %s\n", err);
        free(host_mem);
        return 1;
    }

    uint32_t code_base = seg.hunk_base[0];
    uint32_t data_base = seg.hunk_base[1];
    printf("[J4]   loaded: hunk0 CODE @ 0x%08X (%u B), hunk1 DATA @ 0x%08X (%u B), entry=0x%08X\n",
           code_base, seg.hunk_size[0], data_base, seg.hunk_size[1], seg.entry);

    /* (a) The relocation assert. After HUNK_RELOC32 the DATA slot holds, big-endian,
     *     val = original_addend + CODE_base. */
    uint32_t expect_ptr = DATA_ADDEND + code_base;
    uint32_t got_ptr    = sandbox_be32(&sb, data_base);   /* offset 0 in DATA */
    int reloc_ok = (got_ptr == expect_ptr);
    printf("[J4]   RELOCATION assert: DATA[0] expected 0x%08X (addend 0x%X + CODE_base 0x%08X), got 0x%08X -> %s\n",
           expect_ptr, DATA_ADDEND, code_base, got_ptr, reloc_ok ? "MATCH" : "MISMATCH");

    /* ---------- (2) TRANSLATE + RUN the entry (the [J2] Emu68-emitter path) ---------- */
    uint32_t exit_d0 = 0xFFFFFFFFu;
    if (j4_run_entry(&sb, &seg, &exit_d0, err, sizeof(err)) != 0) {
        printf("[J4] FAIL: run-entry error: %s\n", err);
        j4_run_free();
        free(host_mem);
        return 1;
    }
    int d0_ok = (exit_d0 == EXPECT_D0);
    printf("[J4]   EXECUTED entry through MAP_JIT (Emu68 emitter): d0 = %u (0x%08X), expected %u -> %s\n",
           exit_d0, exit_d0, EXPECT_D0, d0_ok ? "MATCH" : "MISMATCH");
    j4_run_free();

    /* ---------- NEGATIVE CONTROL: skip relocation, the pointer must be WRONG ---------- */
    j4_sandbox sb2;
    j4_sandbox_init(&sb2, host_mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    j4_seglist seg2;
    int ctl_ok = 0;
    if (j4_load_hunks(&sb2, bin, binlen, /*skip_reloc=*/1, &seg2, err, sizeof(err)) == 0) {
        uint32_t raw = sandbox_be32(&sb2, seg2.hunk_base[1]);
        /* With relocation skipped the slot still holds only the raw addend, NOT the
         * relocated runtime pointer — so the relocation assert genuinely bites. */
        ctl_ok = (raw == DATA_ADDEND) && (raw != expect_ptr);
        printf("[J4]   NEGATIVE CONTROL (skip reloc): DATA[0] = 0x%08X (raw addend, unrelocated) -> %s\n",
               raw, ctl_ok ? "correctly WRONG (assert bites)" : "unexpected");
    } else {
        printf("[J4]   NEGATIVE CONTROL load failed: %s\n", err);
    }

    free(host_mem);

    int ok = reloc_ok && d0_ok && ctl_ok;
    if (ok) {
        printf("[J4] PASS: load -> relocate -> sandbox -> translate -> run -> return proven for a real "
               "register-only 68k hunk binary. Relocated DATA[0]=0x%08X (byte-exact big-endian) and the "
               "executed entry returned d0=%u; negative control (skip reloc) is correctly wrong.\n",
               got_ptr, exit_d0);
    } else {
        printf("[J4] FAIL: %s%s%s\n",
               reloc_ok ? "" : "relocation mismatch ",
               d0_ok    ? "" : "d0 mismatch ",
               ctl_ok   ? "" : "negative control did not bite ");
    }
    return ok ? 0 : 1;
}
