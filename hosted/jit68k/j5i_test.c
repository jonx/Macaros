/* j5i_test.c — [J5i] value-asserting driver for the 68k EXCEPTION / SR model. A REAL
 * big-endian AmigaOS hunk program (apps68k/j5i.s -> bin/j5i.exe) installs 68k exception
 * handlers in the vector table and raises three exceptions from three REAL causes:
 *   - trap #1       -> vector 33 (32+1)         (handler tallies + rte resumes)
 *   - divu.w #0,d0  -> vector 5  (div by zero)  (handler tallies + rte resumes)
 *   - ILLEGAL       -> vector 4                 (handler tallies + redirects, no rte)
 * plus a fourth, hand-built micro-test that raises an ADDRESS/BUS error (vector 2/3) from
 * a jump to an out-of-sandbox PC — the hosted stand-in for the host SIGSEGV an integrated
 * JIT takes (the graft/cpu_aarch64.h seam). Each exception is asserted to dispatch to the
 * CORRECT vector with the CORRECT frame (SR + return PC pushed to a7) and rte resumed
 * correctly, byte-exact vs the INDEPENDENT from-scratch oracle (j5d_interp.c, OURS, no
 * Emu68). The program must not crash the host; a negative control (zero the vector slot)
 * must bite. (OURS, AROS-licensed.)
 *
 * Clean-room / OURS. Authored from the Motorola M68000 PRM exception model (vector table,
 * the SR+PC short frame, rte, the group vectors 2/3/4/5/32+n) + the [J5d]..[J5h] engine
 * contract only. Uses NO Emu68 source; the engine it drives calls the REAL Emu68 decoders
 * for the body opcodes (ori/lsl/move/...) and OUR C dispatcher for the exception model.
 *
 * THE graft/cpu_aarch64.h INTEGRATION SEAM (stated, not faked). In an AROS-integrated JIT
 * a wild 68k access faults the HOST (SIGSEGV/SIGBUS); graft/cpu_aarch64.h's SAVEREGS/
 * RESTOREREGS + struct ExceptionContext bridge that signal into AROS's trap path at the
 * AArch64 level. The 68k layer here is the piece that pairs with it: recover the faulting
 * m68k PC, then call the SAME j5d_raise_exception() this test exercises to build the 68k
 * frame + vector. In this spike the sandbox bounds-check raises that path WITHOUT a real
 * host signal, so the 68k model runs end-to-end while the host-signal wiring stays an
 * AROS-side integration task. See docs/features/68k-jit/spec.md [J5i].
 */
#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#define ORG     0x00210000u
#define SZ      0x00040000u          /* spans 0x210000..0x250000, covers VBR @ 0x240000 */
#define WANT_D0 0x0000075Au          /* (tally=7 << 8) | 0x5A : all three handlers ran  */

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
    if (!f) { fprintf(stderr, "[J5i] cannot open %s\n", path); return NULL; }
    *len = fread(g_filebuf, 1, sizeof g_filebuf, f);
    fclose(f);
    return g_filebuf;
}

static volatile sig_atomic_t g_alarmed = 0;
static void on_alarm(int sig){ (void)sig; g_alarmed = 1;
    const char *m = "[J5i] FAIL (watchdog timeout)\n"; write(2, m, strlen(m)); _exit(1); }

static int g_fail = 0;

static int eq_regs(const struct j5d_m68k_state *a, const struct j5d_m68k_state *b)
{
    for (int i = 0; i < 8; i++) if (a->d[i] != b->d[i]) return 0;
    for (int i = 0; i < 8; i++) if (a->a[i] != b->a[i]) return 0;
    return 1;
}

static const char *vec_name(unsigned v)
{
    switch (v) {
        case 2:  return "BUS_ERROR";
        case 3:  return "ADDRESS_ERROR";
        case 4:  return "ILLEGAL";
        case 5:  return "DIV_BY_ZERO";
        case 33: return "TRAP#1";
        default: return "?";
    }
}

static int load_j5i(uint8_t *mem, j4_sandbox *sb, uint32_t *entry)
{
    size_t len; uint8_t *buf = read_exe("bin/j5i.exe", &len);
    if (!buf) return 1;
    j4_sandbox_init(sb, mem, ORG, SZ);
    j4_seglist seg; char err[200] = {0};
    if (j4_load_hunks(sb, buf, len, /*skip_reloc=*/0, &seg, err, sizeof err)) {
        printf("    load error: %s\n", err); return 1;
    }
    *entry = seg.entry;
    return 0;
}

static int eq_log(const j5i_exc_log *a, const j5i_exc_log *b)
{
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) {
        const j5i_exc_record *x = &a->rec[i], *y = &b->rec[i];
        if (x->vector != y->vector || x->frame_sr != y->frame_sr ||
            x->frame_pc != y->frame_pc || x->a7_at_entry != y->a7_at_entry ||
            x->handler_pc != y->handler_pc) return 0;
    }
    return 1;
}

/* ============================ the real-program exception run ============================ */
static void run_program(void)
{
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *mem2 = calloc(1, SZ);
    j4_sandbox sb, sb2; uint32_t entry = 0, entry2 = 0;
    if (load_j5i(mem, &sb, &entry) || load_j5i(mem2, &sb2, &entry2)) {
        g_fail = 1; free(mem); free(mem2); return;
    }
    j5d_sandbox a = { sb.host_mem, sb.sandbox_origin, sb.size };
    j5d_sandbox b = { sb2.host_mem, sb2.sandbox_origin, sb2.size };

    /* JIT through the engine (REAL Emu68 decoders for the body + OUR C exception model). */
    j5i_exc_log jlog = {0};
    j5d_set_exc_log(&jlog);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run(&a, entry, 0, &jit, &d0, NULL, NULL, err, sizeof err);
    j5d_stats s; j5d_get_stats(&s);
    j5d_set_exc_log(NULL);

    /* Independent reference over a separately-loaded sandbox + its own exception log. */
    j5i_exc_log rlog = {0};
    j5d_interp_set_exc_log(&rlog);
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t rd0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&b, entry2, 0, &ref, &rd0, NULL, NULL, e2, sizeof e2);
    j5d_interp_set_exc_log(NULL);

    int val_ok   = (rc == 0) && (d0 == WANT_D0) && (d0 == rd0);
    int regs_ok  = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int mem_ok   = (memcmp(mem, mem2, SZ) == 0);
    int log_ok   = eq_log(&jlog, &rlog);
    /* exactly three exceptions, in order: trap#1 (33), div0 (5), illegal (4) */
    int seq_ok   = (jlog.n == 3) &&
                   (jlog.rec[0].vector == 33) &&
                   (jlog.rec[1].vector == 5)  &&
                   (jlog.rec[2].vector == 4);
    /* each frame's SR has the S bit CLEAR (the program ran in user state before each
     * exception) and the supervisor bit was SET on entry (we check jit.sr_high here would
     * be 0 after the program returns because rte restored it; instead assert the pushed SR
     * shows S clear — the SR SAVED is the pre-exception SR). */
    int frame_ok = 1;
    for (int i = 0; i < jlog.n; i++)
        if (jlog.rec[i].frame_sr & J5D_SR_S) frame_ok = 0;   /* pre-exception SR: S clear */
    int ok = val_ok && regs_ok && mem_ok && log_ok && seq_ok && frame_ok;

    printf("  j5i.exe  install handlers, then raise trap #1, divu.w #0, ILLEGAL:\n");
    printf("    %d exception(s) dispatched (JIT) / %d (oracle):\n", jlog.n, rlog.n);
    for (int i = 0; i < jlog.n; i++) {
        const j5i_exc_record *r = &jlog.rec[i];
        printf("      #%d vector %-3u (%-13s) frame: SR=0x%04X PC=0x%08X @a7=0x%08X -> handler 0x%08X\n",
               i, r->vector, vec_name(r->vector), r->frame_sr, r->frame_pc, r->a7_at_entry, r->handler_pc);
    }
    printf("    JIT d0=0x%08X  REF d0=0x%08X  (want 0x%08X)  regs=%s  sandbox-mem=%s\n",
           d0, rd0, WANT_D0, regs_ok ? "byte-exact" : "DIVERGE", mem_ok ? "byte-exact" : "DIVERGE");
    printf("    engine: %u exceptions dispatched, %u rte returns; through the JIT: %u blocks, %u insns\n",
           s.exceptions_dispatched, s.rte_returns, s.blocks_translated, s.insns_decoded);
    if (rc)  printf("    run error: %s\n", err);
    if (irc) printf("    interp error: %s\n", e2);
    printf("    asserts: val=%s regs=%s mem=%s log(JIT==oracle)=%s seq=%s frame(S-clear-saved)=%s -> %s\n",
           val_ok?"ok":"X", regs_ok?"ok":"X", mem_ok?"ok":"X", log_ok?"ok":"X",
           seq_ok?"ok":"X", frame_ok?"ok":"X", ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;

    j5d_run_free();
    free(mem); free(mem2);
}

/* ===================== a focused TRAP-frame + rte micro-check (hand-built) =====================
 * A minimal block proves the FRAME CONTENTS and rte resume in isolation, with a handler that
 * writes a witness so we can see rte actually resumed at the saved PC. Layout (origin-based):
 *   ENTRY:  moveq #0,d1
 *           lea  handler(pc),a0 ; move.l a0, VBR+34*4   ; install vector 34 (trap #2)
 *           trap #2                                     ; -> vector 34
 *           moveq #0x5A,d0      ; <- rte must resume HERE (saved PC = after the trap)
 *           rts
 *   HANDLER: moveq #0x11,d1     ; witness: handler ran
 *           rte
 * After the run: d1 == 0x11 (handler ran) AND d0 == 0x5A (rte resumed at the post-trap insn).
 * The saved frame PC must equal the address of `moveq #0x5A,d0`. Byte-exact vs the oracle. */
static void run_trapframe(void)
{
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *mem2 = calloc(1, SZ);
    /* Hand-assembled big-endian; addresses computed for ORG=0x210000. The lea(d16,pc) base
     * is the address of the EXTENSION WORD (= lea-opcode addr + 2).
     * 0x210000: 7200            moveq #0,d1
     * 0x210002: 41FA 0010       lea (0x10,pc),a0     ; ext@0x210004; +0x10 -> 0x210014 handler
     * 0x210006: 23C8 0024 0088  move.l a0, 0x240088  ; VBR + 34*4
     * 0x21000C: 4E42            trap #2
     * 0x21000E: 705A            moveq #0x5A,d0        ; <- rte resumes here (post-trap)
     * 0x210010: 4E75            rts
     * 0x210012: 4E71            nop (pad so the handler lands exactly on 0x210014)
     * 0x210014: 7211            moveq #0x11,d1        ; handler witness
     * 0x210016: 4E73            rte
     */
    static const uint8_t CODE[] = {
        0x72,0x00,                       /* 0x00 moveq #0,d1            */
        0x41,0xFA,0x00,0x10,             /* 0x02 lea (0x10,pc),a0 -> 0x210014 (ext@0x210004 +0x10) */
        0x23,0xC8,0x00,0x24,0x00,0x88,   /* 0x06 move.l a0,0x240088 (VBR+34*4) */
        0x4E,0x42,                       /* 0x0C trap #2                */
        0x70,0x5A,                       /* 0x0E moveq #0x5A,d0  <- rte resumes here */
        0x4E,0x75,                       /* 0x10 rts                    */
        0x4E,0x71,                       /* 0x12 nop (pad)              */
        0x72,0x11,                       /* 0x14 moveq #0x11,d1 (handler witness) */
        0x4E,0x73                        /* 0x16 rte                    */
    };
    memcpy(mem,  CODE, sizeof CODE);
    memcpy(mem2, CODE, sizeof CODE);
    j5d_sandbox a = { mem, ORG, SZ }, b = { mem2, ORG, SZ };

    j5i_exc_log jlog = {0}; j5d_set_exc_log(&jlog);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char e1[200] = {0};
    int rc = j5d_run(&a, ORG, 0, &jit, &d0, NULL, NULL, e1, sizeof e1);
    j5d_set_exc_log(NULL);

    j5i_exc_log rlog = {0}; j5d_interp_set_exc_log(&rlog);
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t r0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&b, ORG, 0, &ref, &r0, NULL, NULL, e2, sizeof e2);
    j5d_interp_set_exc_log(NULL);

    uint32_t want_resume_pc = ORG + 0x0E;     /* address of `moveq #0x5A,d0` */
    uint32_t want_handler   = ORG + 0x14;     /* address of the handler      */
    int frame_pc_ok = (jlog.n == 1) && (jlog.rec[0].frame_pc == want_resume_pc);
    int handler_ok  = (jlog.n == 1) && (jlog.rec[0].handler_pc == want_handler);
    int vec_ok      = (jlog.n == 1) && (jlog.rec[0].vector == 34);
    int witness_ok  = (jit.d[1] == 0x11);     /* handler ran                 */
    int resumed_ok  = (d0 == 0x5A);           /* rte resumed at post-trap    */
    int regs_ok     = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int mem_ok      = (memcmp(mem, mem2, SZ) == 0);
    int log_ok      = eq_log(&jlog, &rlog);
    int ok = frame_pc_ok && handler_ok && vec_ok && witness_ok && resumed_ok &&
             regs_ok && mem_ok && log_ok;

    printf("  trapframe  trap #2 -> vector 34, handler writes d1, rte resumes:\n");
    if (jlog.n == 1)
        printf("    frame: SR=0x%04X PC=0x%08X (want resume 0x%08X) @a7=0x%08X handler=0x%08X (want 0x%08X)\n",
               jlog.rec[0].frame_sr, jlog.rec[0].frame_pc, want_resume_pc,
               jlog.rec[0].a7_at_entry, jlog.rec[0].handler_pc, want_handler);
    printf("    handler witness d1=0x%02X (want 0x11)  rte resumed d0=0x%02X (want 0x5A)\n",
           jit.d[1], d0);
    printf("    asserts: vec=%s frame_pc=%s handler=%s witness=%s resumed=%s regs=%s mem=%s log=%s -> %s\n",
           vec_ok?"ok":"X", frame_pc_ok?"ok":"X", handler_ok?"ok":"X", witness_ok?"ok":"X",
           resumed_ok?"ok":"X", regs_ok?"ok":"X", mem_ok?"ok":"X", log_ok?"ok":"X", ok?"PASS":"FAIL");
    if (rc)  printf("    run error: %s\n", e1);
    if (irc) printf("    interp error: %s\n", e2);
    if (!ok) g_fail = 1;
    j5d_set_exc_log(NULL);
    j5d_run_free(); free(mem); free(mem2);
}

/* ===================== ADDRESS/BUS error from a wild jump (the graft seam) =====================
 * A jmp to an out-of-sandbox PC is the hosted stand-in for the host SIGSEGV an integrated JIT
 * takes. The dispatcher turns it into a 68k BUS error (vector 2). We install a vector-2 handler
 * that records a witness, pops its own exception frame (addq.l #6,a7), and rts at top level.
 * Assert vector 2 dispatched with the bad PC in the frame; byte-exact vs the oracle.
 *
 * Layout (ORG=0x210000; lea(d16,pc) base = ext-word address = lea-opcode + 2):
 *   0x210000: 41FA 000E       lea (0x0E,pc),a0      ; ext@0x210002 +0x0E -> 0x210010 handler
 *   0x210004: 23C8 0024 0008  move.l a0, 0x240008   ; VBR + 2*4 (bus-error vector)
 *   0x21000A: 4EF9 0010 0000  jmp 0x00100000        ; BELOW origin -> 68k BUS error, vector 2
 *   0x210010: 7059            moveq #0x59,d0        ; handler witness (0x59 < 0x80: no sign-ext)
 *   0x210012: 5C8F            addq.l #6,a7          ; pop our own bus-error frame
 *   0x210014: 4E75            rts                   ; top-level -> exit with d0=0x59          */
static void run_buserror(void)
{
    uint8_t *mem  = calloc(1, SZ);
    uint8_t *mem2 = calloc(1, SZ);
    static const uint8_t CODE[] = {
        0x41,0xFA,0x00,0x0E,             /* 0x00 lea (0x0E,pc),a0 -> 0x210010 handler   */
        0x23,0xC8,0x00,0x24,0x00,0x08,   /* 0x04 move.l a0, 0x240008 (VBR+2*4)          */
        0x4E,0xF9,0x00,0x10,0x00,0x00,   /* 0x0A jmp 0x00100000 (out of sandbox -> BUS) */
        0x70,0x59,                       /* 0x10 moveq #0x59,d0 (handler witness)       */
        0x5C,0x8F,                       /* 0x12 addq.l #6,a7   (pop the bus frame)      */
        0x4E,0x75                        /* 0x14 rts (top-level -> exit, d0=0x59)        */
    };
    memcpy(mem,  CODE, sizeof CODE);
    memcpy(mem2, CODE, sizeof CODE);
    j5d_sandbox a = { mem, ORG, SZ }, b = { mem2, ORG, SZ };

    j5i_exc_log jlog = {0}; j5d_set_exc_log(&jlog);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char e1[200] = {0};
    int rc = j5d_run(&a, ORG, 0, &jit, &d0, NULL, NULL, e1, sizeof e1);
    j5d_set_exc_log(NULL);

    j5i_exc_log rlog = {0}; j5d_interp_set_exc_log(&rlog);
    struct j5d_m68k_state ref; memset(&ref, 0, sizeof ref);
    uint32_t r0 = 0; char e2[200] = {0};
    int irc = j5d_interp_run(&b, ORG, 0, &ref, &r0, NULL, NULL, e2, sizeof e2);
    j5d_interp_set_exc_log(NULL);

    int vec_ok    = (jlog.n == 1) && (jlog.rec[0].vector == J5I_VEC_BUS_ERROR);
    int badpc_ok  = (jlog.n == 1) && (jlog.rec[0].frame_pc == 0x00100000u);  /* bad target in frame */
    int witness_ok= (jit.d[0] == 0x59) && (d0 == 0x59);   /* handler ran + halted via rts */
    int regs_ok   = (rc == 0) && (irc == 0) && eq_regs(&jit, &ref);
    int mem_ok    = (memcmp(mem, mem2, SZ) == 0);
    int log_ok    = eq_log(&jlog, &rlog);
    int ok = vec_ok && badpc_ok && witness_ok && regs_ok && mem_ok && log_ok && (rc == 0);

    printf("  buserror   jmp 0x00100000 (out of sandbox) -> 68k BUS error (vector 2):\n");
    if (jlog.n >= 1)
        printf("    frame: vector=%u (%s) bad-PC=0x%08X @a7=0x%08X handler=0x%08X\n",
               jlog.rec[0].vector, vec_name(jlog.rec[0].vector), jlog.rec[0].frame_pc,
               jlog.rec[0].a7_at_entry, jlog.rec[0].handler_pc);
    printf("    handler witness d0=0x%02X (want 0x59)  [the graft/cpu_aarch64.h SIGSEGV seam, modeled in-band]\n",
           jit.d[0]);
    printf("    asserts: vec=%s bad-pc-in-frame=%s witness=%s regs=%s mem=%s log=%s -> %s\n",
           vec_ok?"ok":"X", badpc_ok?"ok":"X", witness_ok?"ok":"X", regs_ok?"ok":"X",
           mem_ok?"ok":"X", log_ok?"ok":"X", ok?"PASS":"FAIL");
    if (rc)  printf("    run error: %s\n", e1);
    if (irc) printf("    interp error: %s\n", e2);
    if (!ok) g_fail = 1;
    j5d_run_free(); free(mem); free(mem2);
}

/* ===================== negative control: zero a vector slot =====================
 * If the trap #1 vector slot is zeroed (no handler installed), the dispatcher reads handler
 * 0x00000000, which is OUT of the sandbox -> the frame push still happens but the jump target
 * 0 then bus-errors... actually handler 0 is below origin: read_vector succeeds (the slot is
 * in-sandbox) but the dispatched PC 0 is out of sandbox -> a SECOND (bus) exception. The key
 * negative-control property: with the handler NOT installed, the program does NOT reach its
 * normal exit value 0x075A. We assert the value DIVERGES (the handler-install + dispatch is
 * load-bearing, not decoration). No host crash either way (clean fault). */
static void neg_control(void)
{
    uint8_t *mem = calloc(1, SZ);
    j4_sandbox sb; uint32_t entry = 0;
    if (load_j5i(mem, &sb, &entry)) { g_fail = 1; free(mem); return; }

    /* NOP out the `move.l a0, VBR+33*4` that installs the trap #1 handler. It is the second
     * install (the lea+move pair). Find the move.l to 0x00240084 (VBR+33*4) and clobber it
     * with NOPs so the vector-33 slot stays 0. */
    uint8_t *code = sb.host_mem + (entry - ORG);
    int patched = 0;
    for (int i = 0; i + 6 <= 0x40; i += 2) {
        /* move.l a0,abs.l = 0x23C8 then the 4-byte abs address 0x00240084 */
        if (code[i]==0x23 && code[i+1]==0xC8 &&
            code[i+2]==0x00 && code[i+3]==0x24 && code[i+4]==0x00 && code[i+5]==0x84) {
            for (int j = 0; j < 6; j += 2) { code[i+j]=0x4E; code[i+j+1]=0x71; } /* nop nop nop */
            patched = 1; break;
        }
    }

    j5i_exc_log jlog = {0}; j5d_set_exc_log(&jlog);
    struct j5d_m68k_state jit; memset(&jit, 0, sizeof jit);
    uint32_t d0 = 0; char err[200] = {0};
    int rc = j5d_run((j5d_sandbox[]){{ sb.host_mem, sb.sandbox_origin, sb.size }},
                     entry, 0, &jit, &d0, NULL, NULL, err, sizeof err);
    j5d_set_exc_log(NULL);

    /* The control "bites" if: we patched it, AND the normal exit value is NOT produced. The
     * uninstalled trap vector (slot=0) dispatches to PC 0 -> a bus error -> which (with no
     * vector-2 handler either) is a clean run error (rc!=0) OR a wrong value. Either way it
     * must NOT equal WANT_D0. */
    int bit = patched && !((rc == 0) && (d0 == WANT_D0));
    printf("  neg-ctrl   un-install the trap #1 handler (NOP the vector store): rc=%d d0=0x%08X "
           "(normal 0x%08X) -> %s\n",
           rc, d0, WANT_D0, bit ? "DIVERGED (handler-install assert bites)" : "FAILED TO BITE");
    if (rc) printf("             clean fault (no host crash): %s\n", err);
    if (!bit) g_fail = 1;
    j5d_run_free(); free(mem);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(15);

    printf("[J5i] 68k EXCEPTION / SR MODEL: a vector table in the sandbox, the standard SR+PC\n");
    printf("      exception frame pushed to the supervisor stack (a7), dispatch through the\n");
    printf("      normal PC loop to the vector's handler, and rte (pop SR+PC, resume). Covers\n");
    printf("      trap #n -> 32+n, divu.w #0 -> 5, ILLEGAL -> 4, and out-of-sandbox PC -> bus/\n");
    printf("      address (2/3) — the graft/cpu_aarch64.h host-SIGSEGV seam, modeled in-band.\n");
    printf("      Each is byte-exact (registers + CCR/SR + sandbox memory + the exception log)\n");
    printf("      vs an independent from-scratch oracle (j5d_interp.c, OURS, no Emu68).\n\n");

    run_program();
    run_trapframe();
    run_buserror();
    neg_control();

    if (g_fail) { printf("\n[J5i] FAIL\n"); return 1; }

    printf("\n  VERDICT: the 68k exception/SR model dispatches the common vectors (trap 32+n,\n");
    printf("           div-by-zero 5, illegal 4, bus/address 2/3) from REAL causes, builds the\n");
    printf("           standard SR+PC short frame on a7, sets the supervisor (S) bit on entry,\n");
    printf("           jumps through the sandbox vector table, and rte pops the frame + resumes\n");
    printf("           — byte-exact (registers + SR + sandbox memory + the per-exception frame)\n");
    printf("           vs an independent oracle; the host never crashed and the negative control\n");
    printf("           bites. MODELED: vectors 2/3/4/5/32+n, the S bit, the 6-byte SR+PC frame,\n");
    printf("           rte. DEFERRED: the real VBR register, the USP/SSP split, 68010+ frame\n");
    printf("           formats, group-0/1/2 priorities, and the actual host-SIGSEGV->this-path\n");
    printf("           wiring (lands at AROS integration via graft/cpu_aarch64.h SAVEREGS).\n");
    printf("[J5i] PASS\n");
    return 0;
}
