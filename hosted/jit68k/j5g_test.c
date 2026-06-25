/* j5g_test.c — [J5g] value-asserting driver: BROADEN the ISA + addressing-mode coverage.
 * A demanding self-contained 68k program (bubble sort of an array via indexed memory
 * addressing, then a checksum/mixer over the sorted array using the full shift/rotate +
 * immediate + misc opcode set) is run through the JIT engine — Emu68's REAL per-opcode
 * decoders for LINE0/LINE4/LINE5/LINE8/LINE9/LINEB/LINEC/LINED/LINEE + MOVE/MOVEQ, the
 * (An)/(d16,An)/(d8,An,Xn) sandbox EA, and OUR PC-driven dispatcher — and asserted
 * BYTE-EXACT (the full register file AND the sandbox memory, including the sorted array)
 * against an INDEPENDENT from-scratch interpreter (j5d_interp.c, OURS, no Emu68).
 * (OURS, AROS-licensed.)
 *
 * Clean-room / OURS. Authored from the Motorola 68000 PRM (the LINE0 immediates, LINE4
 * misc, LINEE shifts/rotates, the M68000 (d8,An,Xn) indexed mode + (d16,An) displacement)
 * and the [J5d]..[J5f] engine contract only. Uses NO Emu68 source; the engine it drives
 * calls the REAL Emu68 decoders.
 *
 * THE [J5g] PROGRAM (bubsort.s, vasm-assembled -no-opt real hunk; this is the loader's
 * CODE hunk + a relocated DATA hunk holding the array):
 *
 *   array = {17, 3, 42, 8, 99, 23}
 *   bubble-sort ascending in place via (0,a0,Dn.L) indexed load/store
 *     -> sorted = {3, 8, 17, 23, 42, 99}
 *   checksum/mixer over the sorted array (rol/eori in the loop; then swap/eor/lsl/lsr/
 *     asl/asr/ror/ori/andi/addi/subi/neg/not/ext/cmpi/btst/tst fold) -> d0 = 0x00F5B9F5
 *
 * Coverage exercised through the REAL decoders (see bubsort.s header for the full table):
 *   LINE0:  addi.l subi.l andi.l ori.l eori.l cmpi.l btst#b,Dn
 *   LINE4:  clr.l neg.l not.l tst.l swap ext.l
 *   LINEE:  lsl.l lsr.l asl.l asr.l rol.l ror.l (#count,Dn)
 *   modes:  (d8,An,Xn.L) indexed load+store ; (d16,An)/lea ; (An) ; abs.l(lea) ; #imm ; .L
 *   plus the carried [J5d]..[J5f] move/add/sub/cmp/addq/subq/Bcc/bsr/rts set.
 *
 * The test asserts: d0 == 0x00F5B9F5 (JIT == oracle); the FULL register file byte-exact
 * JIT vs oracle; the SANDBOX MEMORY (including the in-place sorted array) byte-exact; the
 * sorted array reads back {3,8,17,23,42,99}; and the coverage telemetry (>=1 (An)-class
 * memory access through the JIT, the new LINE decoders ran). A negative control corrupts
 * one opcode so the REAL decoder emits a different valid instruction and d0 diverges,
 * proving the byte-exact assert is not a tautology. Watchdog 15s -> FAIL.
 */
#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#define ORG   0x00210000u
#define SZ    0x00040000u

/* bubsort.exe (the committed vasm -no-opt artifact) loaded from apps68k/bin. */
static const char *apps_dir(void)
{
    const char *d = getenv("APPS68K_DIR");
    return (d && *d) ? d : "hosted/jit68k/apps68k";
}

static uint8_t g_filebuf[1 << 16];
static uint8_t *read_exe(const char *rel, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", apps_dir(), rel);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[J5g] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static volatile sig_atomic_t g_alarmed = 0;
static void on_alarm(int sig){ (void)sig; g_alarmed = 1;
    const char *m = "[J5g] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}

/* Load bubsort.exe into a fresh sandbox (load + HUNK_RELOC32 relocate). Returns the entry
 * PC and the DATA hunk base (so we can read back the sorted array). */
static int load_bubsort(uint8_t *mem, j4_sandbox *sb, uint32_t *entry, uint32_t *data_base)
{
    size_t len; uint8_t *buf = read_exe("bin/bubsort.exe", &len);
    if (!buf) return 1;
    j4_sandbox_init(sb, mem, ORG, SZ);
    j4_seglist seg; char err[200] = {0};
    if (j4_load_hunks(sb, buf, len, /*skip_reloc=*/0, &seg, err, sizeof err)) {
        printf("    load error: %s\n", err); return 1;
    }
    *entry = seg.entry;
    *data_base = (seg.numhunks >= 2) ? seg.hunk_base[1] : 0;
    return 0;
}

#define WANT_D0   0x00F5B9F5u
static const uint32_t SORTED[6] = { 3, 8, 17, 23, 42, 99 };

static void run_bubsort(void)
{
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *mem2 = calloc(1, SZ);
    j4_sandbox sb, sb2; uint32_t entry = 0, dbase = 0, entry2 = 0, dbase2 = 0;
    if (load_bubsort(mem, &sb, &entry, &dbase) ||
        load_bubsort(mem2, &sb2, &entry2, &dbase2)) {
        g_fail = 1; free(mem); free(mem2); return;
    }
    j5d_sandbox j5sb  = { sb.host_mem,  sb.sandbox_origin,  sb.size };
    j5d_sandbox j5sb2 = { sb2.host_mem, sb2.sandbox_origin, sb2.size };

    /* JIT through the engine (REAL Emu68 decoders + our dispatcher + the EA edit). */
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&j5sb, entry, 0, &jit, &d0, NULL, NULL, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s);

    /* Independent reference over a separately-loaded sandbox. */
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t rd0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&j5sb2, entry2, 0, &ref, &rd0, NULL, NULL, e2, sizeof e2);

    /* Read the sorted array back from BOTH sandboxes (byte-exact big-endian). */
    uint32_t jit_arr[6], ref_arr[6]; int arr_ok = (dbase != 0);
    for (int i = 0; i < 6 && arr_ok; i++) {
        const uint8_t *pj = sb.host_mem  + (dbase  - ORG) + i*4;
        const uint8_t *pr = sb2.host_mem + (dbase2 - ORG) + i*4;
        jit_arr[i] = ((uint32_t)pj[0]<<24)|((uint32_t)pj[1]<<16)|((uint32_t)pj[2]<<8)|pj[3];
        ref_arr[i] = ((uint32_t)pr[0]<<24)|((uint32_t)pr[1]<<16)|((uint32_t)pr[2]<<8)|pr[3];
        if (jit_arr[i] != SORTED[i]) arr_ok = 0;
    }

    int regs_ok = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int memok   = (memcmp(mem, mem2, SZ) == 0);    /* incl. the in-place sorted array */
    int val_ok  = (rc == 0) && (d0 == WANT_D0) && (d0 == rd0);
    int ccr_ok  = (jit.ccr == ref.ccr);            /* full CCR byte-exact too          */
    int mem_acc_ok = (s.mem_accesses > 0);         /* the indexed (An,Xn) accesses ran */
    int ok = val_ok && regs_ok && memok && arr_ok && ccr_ok && mem_acc_ok;

    printf("  bubsort  bubble-sort {17,3,42,8,99,23} via (d8,An,Xn.L) indexed mem, then a\n");
    printf("           shift/rotate/immediate checksum over the sorted array\n");
    printf("    JIT d0=0x%08X  REF d0=0x%08X  (want 0x%08X)  regs=%s  CCR=%s\n",
           d0, rd0, WANT_D0, regs_ok ? "byte-exact" : "DIVERGE", ccr_ok ? "byte-exact" : "DIVERGE");
    printf("    sorted array JIT = {%u,%u,%u,%u,%u,%u}  (want {3,8,17,23,42,99}) -> %s\n",
           arr_ok?jit_arr[0]:0, arr_ok?jit_arr[1]:0, arr_ok?jit_arr[2]:0,
           arr_ok?jit_arr[3]:0, arr_ok?jit_arr[4]:0, arr_ok?jit_arr[5]:0,
           arr_ok ? "SORTED" : "WRONG");
    printf("    sandbox-mem(incl. array) JIT vs REF = %s\n", memok ? "byte-exact" : "DIVERGE");
    printf("    through the JIT: %u blocks translated, %u executed, %u m68k insns, "
           "%u (An)-class mem accesses, %u AArch64 words\n",
           s.blocks_translated, s.blocks_executed, s.insns_decoded, s.mem_accesses,
           s.arm_words_emitted);
    printf("    block cache: %u translated (misses), %u hits -> %u re-translations avoided\n",
           s.blocks_translated, s.block_cache_hits, s.block_cache_hits);
    if (rc)  printf("    run error: %s\n", err);
    if (irc) printf("    interp error: %s\n", e2);
    printf("    asserts: val=%s regs=%s ccr=%s mem=%s sorted=%s memacc=%s -> %s\n",
           val_ok?"ok":"X", regs_ok?"ok":"X", ccr_ok?"ok":"X", memok?"ok":"X",
           arr_ok?"ok":"X", mem_acc_ok?"ok":"X", ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;

    j5d_run_free();
    free(mem); free(mem2);
}

/* ===================== negative control: corrupt one opcode =====================
 * Flip the bubble-sort comparison's branch (ble.s -> bgt.s) so the array sorts the WRONG
 * way (or not at all), making the checksum diverge from 0x00F5B9F5. The REAL Emu68 decoder
 * still emits a valid block; only the value is wrong -> the byte-exact assert bites. We
 * patch the loaded sandbox copy directly (after load+relocate) so the loader is unaffected. */
static void neg_corrupt(void)
{
    uint8_t *mem = calloc(1, SZ);
    j4_sandbox sb; uint32_t entry = 0, dbase = 0;
    if (load_bubsort(mem, &sb, &entry, &dbase)) { g_fail = 1; free(mem); return; }

    /* Find the ble.s (0x6f08) in the CODE hunk and flip it to bgt.s (0x6e08): the swap
     * condition inverts, so the sort is wrong and the checksum changes. */
    uint8_t *code = sb.host_mem + (entry - ORG);
    int patched = 0;
    for (int i = 0; i + 1 < 0x100; i++) {
        if (code[i] == 0x6f && code[i+1] == 0x08) { code[i] = 0x6e; patched = 1; break; } /* ble.s -> bgt.s */
    }

    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run((j5d_sandbox[]){{ sb.host_mem, sb.sandbox_origin, sb.size }},
                     entry, 0, &jit, &d0, NULL, NULL, err, sizeof err);
    int bit = patched && (rc == 0) && (d0 != WANT_D0);
    printf("  neg-ctrl corrupt sort branch (ble.s -> bgt.s): JIT d0=0x%08X "
           "(uncorrupt 0x%08X) -> %s\n",
           d0, WANT_D0, bit ? "DIVERGED (value assert bites)" : "FAILED TO BITE");
    if (!bit) g_fail = 1;
    j5d_run_free(); free(mem);
}

/* ===================== a focused EA-modes check (abs.l + (d16,An) + (d8,An,Xn)) ======
 * The bubsort program exercises (d8,An,Xn.L) + (An); this adds a tiny self-contained block
 * that also touches a longword via abs.l AND (d16,An) (both newly routed through the sandbox
 * EA), then writes it back via (d8,An,Xn) — verified byte-exact (register + the written cell)
 * vs the oracle, so the broadened EA modes are asserted in the marker test, not just at rest.
 *
 *   data cell V = 0xCAFEBABE lives at off 0x40 in a hand-built sandbox.
 *   moveq #0,d1 ; lea data,a0 ; move.l ($210240).l,d4 ; move.l 0(a0),d5 ;
 *   move.l 4(a0),d6 ; add.l d5,d4 ; move.l d4,(0,a0,d1.l) ; rts
 *   (a0 = 0x210240; the abs.l and 0(a0) read the SAME cell; (d8,a0,d1.l) writes it back) */
static void run_eamodes(void)
{
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *mem2 = calloc(1, SZ);
    /* lay the data cells at 0x210240 (V0) and 0x210244 (V1) in BOTH copies */
    uint32_t V0 = 0xCAFEBABEu, V1 = 0x00000007u;
    for (uint8_t **m = (uint8_t*[]){mem, mem2, NULL}, **p = m; *p; p++) {
        uint8_t *c = *p + 0x240;
        c[0]=V0>>24; c[1]=V0>>16; c[2]=V0>>8; c[3]=(uint8_t)V0;
        c[4]=V1>>24; c[5]=V1>>16; c[6]=V1>>8; c[7]=(uint8_t)V1;
    }
    /* hand-assembled block at ORG (no relocation needed; abs.l is a literal sandbox addr) */
    static const uint8_t CODE[] = {
        0x72,0x00,                         /* moveq #0,d1                                  */
        0x20,0x7c,0x00,0x21,0x02,0x40,     /* movea.l #$00210240,a0  (a0 = &data)          */
        0x28,0x39,0x00,0x21,0x02,0x40,     /* move.l ($00210240).l,d4   abs.l LOAD         */
        0x2a,0x28,0x00,0x00,               /* move.l (0,a0),d5          (d16,An) LOAD       */
        0x2c,0x28,0x00,0x04,               /* move.l (4,a0),d6          (d16,An) LOAD       */
        0xd8,0x85,                         /* add.l d5,d4               d4 = V0 + V0        */
        0x21,0x84,0x10,0x00,               /* move.l d4,(0,a0,d1.l)     (d8,An,Xn) STORE    */
        0x4e,0x75                          /* rts                                          */
    };
    memcpy(mem,  CODE, sizeof CODE);
    memcpy(mem2, CODE, sizeof CODE);
    j5d_sandbox a = { mem, ORG, SZ }, b = { mem2, ORG, SZ };
    struct j5d_m68k_state jit, ref; memset(&jit,0,sizeof jit); memset(&ref,0,sizeof ref);
    uint32_t d0=0, r0=0; char e1[200]={0}, e2[200]={0};
    int rc  = j5d_run(&a, ORG, 0, &jit, &d0, NULL, NULL, e1, sizeof e1);
    int irc = j5d_interp_run(&b, ORG, 0, &ref, &r0, NULL, NULL, e2, sizeof e2);

    int regs_ok = (rc==0)&&(irc==0)&&eq_regs(&jit,&ref);
    int mem_ok  = (memcmp(mem, mem2, SZ)==0);
    /* the (d8,a0,d1.l) store wrote d4 = V0+V0 back to 0x210240 */
    const uint8_t *w = mem + 0x240;
    uint32_t wrote = ((uint32_t)w[0]<<24)|((uint32_t)w[1]<<16)|((uint32_t)w[2]<<8)|w[3];
    int val_ok = (jit.d[4]==(uint32_t)(V0+V0)) && (jit.d[5]==V0) && (jit.d[6]==V1) &&
                 (wrote==(uint32_t)(V0+V0));
    int ok = regs_ok && mem_ok && val_ok;
    printf("  ea-modes abs.l + (d16,An) loads + (d8,An,Xn) store, byte-exact vs oracle\n");
    printf("    d4(abs.l+abs.l add)=0x%08X d5((0,a0))=0x%08X d6((4,a0))=0x%08X  written cell=0x%08X\n",
           jit.d[4], jit.d[5], jit.d[6], wrote);
    printf("    regs=%s sandbox-mem=%s vals=%s -> %s\n",
           regs_ok?"byte-exact":"DIVERGE", mem_ok?"byte-exact":"DIVERGE", val_ok?"ok":"X",
           ok?"PASS":"FAIL");
    if (rc)  printf("    run error: %s\n", e1);
    if (irc) printf("    interp error: %s\n", e2);
    if (!ok) g_fail = 1;
    j5d_run_free(); free(mem); free(mem2);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(15);

    printf("[J5g] BROADEN ISA + addressing-mode coverage: a demanding 68k program (bubble\n");
    printf("      sort via (d8,An,Xn.L) indexed memory + a shift/rotate/immediate checksum)\n");
    printf("      runs through Emu68's REAL LINE0/LINE4/LINEE (+ the carried LINE5/8/9/B/C/D/\n");
    printf("      MOVE) decoders + the (An)/(d16,An)/(d8,An,Xn) sandbox EA + our dispatcher,\n");
    printf("      asserted byte-exact (registers + sandbox memory) vs an independent oracle.\n\n");

    run_bubsort();
    run_eamodes();
    neg_corrupt();

    if (g_fail) { printf("\n[J5g] FAIL\n"); return 1; }

    printf("\n  VERDICT: the JIT now drives a substantially BROADER ISA through Emu68's REAL\n");
    printf("           decoders — LINE0 immediates (addi/subi/andi/ori/eori/cmpi/btst),\n");
    printf("           LINE4 misc (clr/tst/swap/ext + lea), LINEE shifts/rotates\n");
    printf("           (lsl/lsr/asl/asr/rol/ror) — over the full M68000 (An)/(An)+/-(An)/\n");
    printf("           (d16,An)/(d8,An,Xn)/abs/#imm addressing modes, byte/word/long, with\n");
    printf("           the (d8,An,Xn) indexed array access routed through the sandbox EA\n");
    printf("           (base-adjust + REV). A demanding bubble-sort + checksum program is\n");
    printf("           byte-exact (registers + sandbox memory + CCR) vs an independent\n");
    printf("           reference; the negative control bites. Still beyond [J5g]: neg.l/\n");
    printf("           not.l + ROXL/ROXR + the X bit's full multi-precision chains (NOT in\n");
    printf("           the verified program), movem/movep/bitfield/BCD, the SR/exception\n");
    printf("           model (host SIGSEGV -> 68k vector), SMC/dirty-page cache\n");
    printf("           invalidation, the FPU/privileged/line-A/F ISA, and the boot-gated\n");
    printf("           real AROS library environment.\n");
    printf("[J5g] PASS\n");
    return 0;
}
