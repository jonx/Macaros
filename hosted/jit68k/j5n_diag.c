/* j5n_diag.c — [J5n] the 68k-JIT DIAGNOSTICS subsystem (OURS, AROS-licensed). See
 * j5n_diag.h for the contract. Implements the j5d_fault funnel, the two-level REPORT.txt,
 * the crash bundle (dir + tar.gz + README/MANIFEST/REPRODUCE/snapshot), the LOUD banner,
 * the flight recorder, the host-signal safety net, the differential lockstep + replay-to-N
 * step hook, the minimal 68k disassembler, and snapshot load. No Emu68 source.
 *
 * The funnel never re-enters the JIT and is best-effort on the crash path (we are already
 * dying): it uses stdio for the file build under normal in-band faults, and degrades to a
 * minimal write()-based banner inside a real signal handler (documented async caveat). */
#include "j5n_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>
#include <execinfo.h>

/* The engine git commit + build config are baked in by the Makefile via -D; provide
 * fallbacks so this file also compiles standalone. */
#ifndef J5N_GIT_COMMIT
#define J5N_GIT_COMMIT "(unknown — built without -DJ5N_GIT_COMMIT)"
#endif
#ifndef J5N_BUILD_CONFIG
#define J5N_BUILD_CONFIG "clang -arch arm64 -O2 (hosted darwin-aarch64)"
#endif

/* ======================== config registration (the side-channel) ======================= */
static j5n_diag *g_engine_diag = NULL;   /* the engine's view (j5d_engine.c reads it)   */
static j5n_diag *g_interp_diag = NULL;   /* the oracle's view (j5d_interp.c calls step)  */

/* the interp's weak step-hook setter (j5d_interp.c). */
typedef int (*j5n_step_hook_fn)(void *diag, const struct j5d_m68k_state *st,
                                j5d_sandbox *sb, uint32_t pc, uint16_t op);
void j5d_interp_set_step_hook(j5n_step_hook_fn fn, void *diag);

/* the trampoline that adapts the interp hook signature to j5n_diag_step. */
static int interp_step_trampoline(void *diag, const struct j5d_m68k_state *st,
                                  j5d_sandbox *sb, uint32_t pc, uint16_t op)
{
    return j5n_diag_step((j5n_diag *)diag, st, sb, pc, op);
}

void j5d_set_diag(j5n_diag *d) { g_engine_diag = d; }
void j5d_interp_set_diag(j5n_diag *d)
{
    g_interp_diag = d;
    if (d) j5d_interp_set_step_hook(interp_step_trampoline, d);
    else   j5d_interp_set_step_hook(NULL, NULL);
}
j5n_diag *j5d_get_diag(void) { return g_engine_diag; }

const char *j5n_fault_kind_name(j5n_fault_kind k)
{
    switch (k) {
        case J5N_FAULT_OOB_READ:    return "oob-read";
        case J5N_FAULT_OOB_WRITE:   return "oob-write";
        case J5N_FAULT_BUS:         return "bus-error";
        case J5N_FAULT_ADDRESS:     return "address-error";
        case J5N_FAULT_ILLEGAL:     return "illegal-insn";
        case J5N_FAULT_DIVZERO:     return "div-by-zero";
        case J5N_FAULT_HOST_SIGNAL: return "host-signal";
        case J5N_FAULT_DIVERGE:     return "jit-diverge";
        case J5N_FAULT_ENGINE:      return "engine-error";
        default:                    return "unknown";
    }
}

void j5n_diag_init(j5n_diag *d, const uint8_t *prog, size_t prog_len, j5d_sandbox *sb,
                   uint32_t entry_pc, uint32_t a6_libbase, const j5n_symtab *symtab)
{
    memset(d, 0, sizeof(*d));
    d->prog = prog; d->prog_len = prog_len; d->sb = sb;
    d->entry_pc = entry_pc; d->a6_libbase = a6_libbase; d->symtab = symtab;
    d->crash_dir = getenv("JIT68K_CRASH_DIR");           /* may be NULL -> <cwd>/crash  */
    if (getenv("JIT68K_DIFF"))  d->diff_enabled  = (atoi(getenv("JIT68K_DIFF")) != 0);
    const char *rt = getenv("JIT68K_RUNTO");
    if (rt && *rt) { d->runto_enabled = 1; d->runto_n = strtoull(rt, NULL, 0); }
}

/* big-endian word read + a minimal instruction-LENGTH walker (used by the flight recorder
 * to lay out a block's instruction trail; heuristic, bounded — see insn_len note below). */
static uint16_t dbe16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  dbe16s(const uint8_t *p){ return (int16_t)dbe16(p); }
static unsigned insn_len(uint16_t op, uint16_t ext)
{
    (void)ext;
    if ((op & 0xF100u) == 0x7000u) return 2;             /* moveq               */
    if ((op & 0xF000u) == 0x6000u) {                     /* Bcc/BRA/BSR         */
        int8_t d = (int8_t)(op & 0xFF);
        if (d == 0)            return 4;                 /* .W                  */
        if ((uint8_t)d==0xFF)  return 6;                 /* .L                  */
        return 2;                                        /* .B                  */
    }
    if ((op & 0xF000u) == 0x5000u) return 2;             /* addq/subq (reg form)*/
    if ((op & 0xF0FFu) == 0x00BCu) return 6;             /* op.l #imm32,Dn      */
    if ((op & 0xF000u) >= 0x8000u && (op & 0xF000u) <= 0xD000u) return 2;
    if ((op & 0xF000u) == 0x2000u || (op & 0xF000u) == 0x3000u ||
        (op & 0xF000u) == 0x1000u) return 2;             /* move reg-direct     */
    return 2;
}

/* ============================== the flight recorder ============================== */
void j5n_flight_push(j5n_diag *d, uint32_t pc, uint16_t op)
{
    if (!d) return;
    uint64_t i = d->flight.head % J5N_FLIGHT_N;
    d->flight.pc[i] = pc;
    d->flight.op[i] = op;
    d->flight.head++;
}

/* Lay out a block's body instructions in the flight recorder (the JIT runs per block, so the
 * engine hands the block range here and we step it instruction by instruction). The
 * terminator at end_pc is recorded too — it's the last thing that executed before the fault. */
void j5n_diag_record_block(j5n_diag *d, j5d_sandbox *sb, uint32_t pc, uint32_t end_pc)
{
    if (!d || !sb) return;
    uint32_t p = pc;
    int guard = 0;
    while (p <= end_pc && guard++ < 300) {
        if (p < sb->origin || (uint64_t)(p - sb->origin) + 2 > sb->size) break;
        const uint8_t *ip = sb->host_mem + (p - sb->origin);
        uint16_t op = ((uint16_t)ip[0] << 8) | ip[1];
        j5n_flight_push(d, p, op);
        if (p == end_pc) break;                  /* the terminator: recorded, stop */
        uint16_t ext = 0;
        if ((uint64_t)(p - sb->origin) + 4 <= sb->size)
            ext = ((uint16_t)ip[2] << 8) | ip[3];
        p += insn_len(op, ext);
    }
}

/* ============================== the minimal 68k disassembler ============================
 * Enough to name the common corpus + fault opcodes; everything else -> dc.w. Authored from
 * the M68000 PRM opcode map. Not a full disassembler (honest scope) — a readable mnemonic.*/
void j5n_disasm(const uint8_t *code, uint32_t pc, uint32_t origin, uint32_t size,
                char *out, size_t outlen)
{
    if (pc < origin || (uint64_t)(pc - origin) + 2 > size) {
        snprintf(out, outlen, "<pc out of sandbox>");
        return;
    }
    const uint8_t *ip = code + (pc - origin);
    uint16_t op = dbe16(ip);
    unsigned reg = (op >> 9) & 7u, srcreg = op & 7u;

    if ((op & 0xF100u) == 0x7000u) { snprintf(out, outlen, "moveq #%d,d%u", (int8_t)(op & 0xFF), reg); return; }
    if (op == 0x4E75u) { snprintf(out, outlen, "rts"); return; }
    if (op == 0x4E73u) { snprintf(out, outlen, "rte"); return; }
    if (op == 0x4E71u) { snprintf(out, outlen, "nop"); return; }
    if (op == 0x4AFCu) { snprintf(out, outlen, "illegal"); return; }
    if ((op & 0xFFF0u) == 0x4E40u) { snprintf(out, outlen, "trap #%u", op & 0xFu); return; }
    if ((op & 0xFFC0u) == 0x80C0u) { snprintf(out, outlen, "divu.w <ea>,d%u", reg); return; }
    if ((op & 0xFFC0u) == 0x81C0u) { snprintf(out, outlen, "divs.w <ea>,d%u", reg); return; }
    if (op == 0x4EB9u) { snprintf(out, outlen, "jsr $%08X", (pc-origin)+4 <= size ? dbe16(ip+2)<<16 | dbe16(ip+4) : 0); return; }
    if (op == 0x4EF9u) { snprintf(out, outlen, "jmp $%08X", (pc-origin)+4 <= size ? dbe16(ip+2)<<16 | dbe16(ip+4) : 0); return; }
    if (op == 0x4EAEu) { snprintf(out, outlen, "jsr %d(a6)  ; library LVO", dbe16s(ip+2)); return; }
    if ((op & 0xFFF8u) == 0x4E90u) { snprintf(out, outlen, "jsr (a%u)", srcreg); return; }
    if ((op & 0xFFF8u) == 0x4ED0u) { snprintf(out, outlen, "jmp (a%u)", srcreg); return; }
    if ((op & 0xF000u) == 0x6000u) {
        unsigned cc = (op >> 8) & 0xFu;
        static const char *ccn[16] = {"ra","sr","hi","ls","cc","cs","ne","eq",
                                      "vc","vs","pl","mi","ge","lt","gt","le"};
        int8_t d8 = (int8_t)(op & 0xFF);
        snprintf(out, outlen, "b%s.%c", ccn[cc], d8 ? 's' : 'w');
        return;
    }
    if ((op & 0xF1C0u) == 0x2040u) { snprintf(out, outlen, "movea.l <ea>,a%u", reg); return; }
    if ((op & 0xF000u) == 0x2000u) { snprintf(out, outlen, "move.l <ea>,<ea>"); return; }
    if ((op & 0xF000u) == 0x3000u) { snprintf(out, outlen, "move.w <ea>,<ea>"); return; }
    if ((op & 0xF000u) == 0x1000u) { snprintf(out, outlen, "move.b <ea>,<ea>"); return; }
    if ((op & 0xF000u) == 0xD000u) { snprintf(out, outlen, "add.* <ea>,d%u", reg); return; }
    if ((op & 0xF000u) == 0x9000u) { snprintf(out, outlen, "sub.* <ea>,d%u", reg); return; }
    if ((op & 0xF000u) == 0xB000u) { snprintf(out, outlen, "cmp/eor.* <ea>,d%u", reg); return; }
    if ((op & 0xF000u) == 0xC000u) { snprintf(out, outlen, "and/mul.* <ea>,d%u", reg); return; }
    if ((op & 0xF000u) == 0x8000u) { snprintf(out, outlen, "or.* <ea>,d%u", reg); return; }
    if ((op & 0xFF00u) == 0x5000u) { snprintf(out, outlen, "addq.* #n,<ea>"); return; }
    if ((op & 0xFF00u) == 0x5100u) { snprintf(out, outlen, "subq.* #n,<ea>"); return; }
    if ((op & 0xF138u) == 0x0108u) { snprintf(out, outlen, "movep"); return; }
    if ((op & 0xFFB8u) == 0x48A8u) { snprintf(out, outlen, "movem <list>,<ea>"); return; }
    if ((op & 0xFFB8u) == 0x4CA8u) { snprintf(out, outlen, "movem <ea>,<list>"); return; }
    if ((op & 0xF1C0u) == 0x41C0u) { snprintf(out, outlen, "lea <ea>,a%u", reg); return; }
    snprintf(out, outlen, "dc.w $%04X", op);
}

/* The opcode-word group (1..3 words) of the instruction at pc, for the report. */
static void opwords(const j5d_sandbox *sb, uint32_t pc, char *out, size_t outlen)
{
    if (pc < sb->origin || (uint64_t)(pc - sb->origin) + 2 > sb->size) {
        snprintf(out, outlen, "<out of sandbox>");
        return;
    }
    const uint8_t *ip = sb->host_mem + (pc - sb->origin);
    size_t avail = sb->size - (pc - sb->origin);
    char *p = out; size_t left = outlen;
    int nw = (avail >= 6) ? 3 : (avail >= 4 ? 2 : 1);
    for (int i = 0; i < nw; i++) {
        int k = snprintf(p, left, "%s%04X", i ? " " : "", dbe16(ip + i*2));
        if (k < 0 || (size_t)k >= left) break;
        p += k; left -= (size_t)k;
    }
}

/* =============================== the SHA-256 (for program.sha256) ======================
 * A compact, self-contained SHA-256 (public-domain construction; OURS implementation) so
 * the bundle is verifiable without external tools. */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_ctx;
static uint32_t ror(uint32_t x, int n){ return (x >> n) | (x << (32 - n)); }
static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    static const uint32_t K[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25), ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+K[i]+w[i];
        uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22), mj=(a&b)^(a&cc)^(b&cc);
        uint32_t t2=S0+mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}
static void sha256(const uint8_t *data, size_t len, char hex[65])
{
    sha256_ctx c = {{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19},0,{0},0};
    size_t i = 0;
    c.len = len;
    while (i + 64 <= len) { sha256_block(&c, data + i); i += 64; }
    uint8_t tail[128]; size_t t = len - i;
    memcpy(tail, data + i, t);
    tail[t++] = 0x80;
    size_t pad = (t <= 56) ? 64 : 128;
    while (t < pad - 8) tail[t++] = 0;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 7; k >= 0; k--) tail[t++] = (uint8_t)(bits >> (k*8));
    sha256_block(&c, tail);
    if (pad == 128) sha256_block(&c, tail + 64);
    for (int k = 0; k < 8; k++)
        snprintf(hex + k*8, 9, "%08x", c.h[k]);
    hex[64] = '\0';
}

/* =============================== the architectural SR ===============================
 * j5d_pack_sr lives in the engine; declare it so the report shows the standard 68k SR. */
uint16_t j5d_pack_sr(const struct j5d_m68k_state *st);

/* ============================ the 68k call-stack walk ============================
 * Walk the guest return stack: A7 points at the live SP; we scan upward toward the initial
 * SP (top of sandbox) and treat each in-code longword as a possible return address, mapping
 * it to the enclosing symbol. This is heuristic (no frame pointer guaranteed) but names the
 * call chain for the common bsr/jsr corpus. Honest: a precise unwinder needs frame info. */
static void write_68k_stack(FILE *f, const struct j5d_m68k_state *st, j5d_sandbox *sb,
                            const j5n_symtab *symtab)
{
    uint32_t sp  = st->a[7];
    uint32_t top = (sb->origin + sb->size) & ~0xFu;
    fprintf(f, "  68k call stack (return-stack walk from a7=0x%08X up to top 0x%08X):\n", sp, top);
    /* frame 0 = the faulting PC itself. */
    uint32_t pcd = 0; const j5n_sym *s0 = j5n_symbols_lookup(symtab, st->pc, 0x10000, &pcd);
    fprintf(f, "    #0  pc=0x%08X  %s%s%+d>\n", st->pc,
            s0 ? "<" : "<no symbol", s0 ? s0->name : "", s0 ? (int)pcd : 0);
    /* clamp the walk to the sandbox: a corrupt/zero a7 must NOT read out of host bounds (we
     * are already on a crash path; a wild read here would fault again). If sp is outside the
     * sandbox there is no recoverable 68k stack to walk. */
    if (!sb->host_mem || sp < sb->origin || sp >= top) {
        fprintf(f, "    (a7 is outside the sandbox — no 68k return stack to walk)\n");
        return;
    }
    int frame = 1;
    for (uint32_t a = sp; a + 4 <= top && frame < 32; a += 2) {
        const uint8_t *p = sb->host_mem + (a - sb->origin);
        uint32_t cand = ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
        /* a plausible return address: even, inside the sandbox code region. */
        if ((cand & 1u) || cand < sb->origin || cand >= top) continue;
        uint32_t d = 0; const j5n_sym *s = j5n_symbols_lookup(symtab, cand, 0x4000, &d);
        if (!s && symtab && symtab->n) continue;   /* with symbols, only show resolved ones */
        fprintf(f, "    #%-2d ret=0x%08X  %s%s%+d>%s\n", frame++, cand,
                s ? "<" : "<", s ? s->name : "?", s ? (int)d : 0, s ? "" : " (unresolved)");
        if (frame >= 12) { fprintf(f, "    ... (stack walk capped)\n"); break; }
    }
    if (frame == 1) fprintf(f, "    (no further return addresses recovered)\n");
}

/* ============================ REPORT.txt — the two-level dump ============================ */
static void write_report(FILE *f, j5n_fault_kind kind, const char *detail,
                         const struct j5d_m68k_state *st, j5d_sandbox *sb,
                         const j5n_hostregs *host, j5n_diag *d)
{
    char dis[128], ow[64];
    j5n_disasm(sb->host_mem, st->pc, sb->origin, sb->size, dis, sizeof dis);
    opwords(sb, st->pc, ow, sizeof ow);
    uint16_t sr = j5d_pack_sr(st);

    fprintf(f, "================================================================\n");
    fprintf(f, " AROS 68k JIT — CRASH REPORT\n");
    fprintf(f, "================================================================\n\n");

    /* ----- Level 1: the COORDINATE ----- */
    fprintf(f, "FAULT\n");
    fprintf(f, "  kind        : %s\n", j5n_fault_kind_name(kind));
    fprintf(f, "  detail      : %s\n", detail ? detail : "(none)");
    fprintf(f, "  instruction#: %llu  (the deterministic global 68k instruction number N)\n",
            (unsigned long long)(d ? d->insn_number : 0));
    fprintf(f, "  68k PC      : 0x%08X\n", st->pc);
    {
        uint32_t pd = 0; const j5n_sym *s = j5n_symbols_lookup(d ? d->symtab : NULL, st->pc, 0x10000, &pd);
        if (s) fprintf(f, "  in function : %s + 0x%X\n", s->name, pd);
    }
    fprintf(f, "  host context: %s\n\n", (host && host->have) ? "captured (real host signal)"
                                                              : "in-band 68k fault (no host signal)");

    /* ----- Level 2: the faulting 68k INSTRUCTION ----- */
    fprintf(f, "FAULTING 68k INSTRUCTION\n");
    fprintf(f, "  opcode words: %s\n", ow);
    fprintf(f, "  disassembly : %s\n\n", dis);

    /* ----- 68k registers ----- */
    fprintf(f, "68k REGISTERS\n");
    for (int i = 0; i < 8; i++)
        fprintf(f, "  D%d=0x%08X%s", i, st->d[i], (i % 4 == 3) ? "\n" : "  ");
    for (int i = 0; i < 8; i++)
        fprintf(f, "  A%d=0x%08X%s", i, st->a[i], (i % 4 == 3) ? "\n" : "  ");
    fprintf(f, "  PC=0x%08X  SR=0x%04X  CCR(internal)=0x%02X  [%s%s%s%s%s]\n\n",
            st->pc, sr, st->ccr & 0x1F,
            (st->ccr & J5D_CCR_X) ? "X" : "-", (st->ccr & J5D_CCR_N) ? "N" : "-",
            (st->ccr & J5D_CCR_Z) ? "Z" : "-", (st->ccr & J5D_CCR_V) ? "V" : "-",
            (st->ccr & J5D_CCR_C) ? "C" : "-");

    /* ----- host AArch64 registers ----- */
    fprintf(f, "HOST AArch64 REGISTERS\n");
    if (host && host->have) {
        for (int i = 0; i < 31; i++)
            fprintf(f, "  x%-2d=0x%016llX%s", i, (unsigned long long)host->x[i],
                    (i % 2 == 1) ? "\n" : "  ");
        fprintf(f, "  sp =0x%016llX  pc =0x%016llX  cpsr=0x%08X\n\n",
                (unsigned long long)host->sp, (unsigned long long)host->pc, host->cpsr);
    } else {
        fprintf(f, "  (no host signal context — this fault was detected cleanly in-band by the\n");
        fprintf(f, "   dispatcher's bounds/decode checks, not via a host SIGSEGV. The host-signal\n");
        fprintf(f, "   net is installed; a genuine host fault would capture x0-x30/sp/pc here.)\n\n");
    }

    /* ----- the TWO stacks ----- */
    write_68k_stack(f, st, sb, d ? d->symtab : NULL);
    fprintf(f, "\n  native host backtrace (the crashing thread):\n");
    {
        void *bt[64];
        int n = backtrace(bt, 64);
        char **syms = backtrace_symbols(bt, n);
        if (syms) {
            for (int i = 0; i < n; i++) fprintf(f, "    %s\n", syms[i]);
            free(syms);
        } else {
            for (int i = 0; i < n; i++) fprintf(f, "    %p\n", bt[i]);
        }
    }
    fprintf(f, "\n");

    /* ----- the flight recorder ----- */
    fprintf(f, "FLIGHT RECORDER (last %d instructions executed before the fault)\n",
            J5N_FLIGHT_N);
    if (d && d->flight.head) {
        uint64_t total = d->flight.head;
        uint64_t start = (total > J5N_FLIGHT_N) ? (total - J5N_FLIGHT_N) : 0;
        for (uint64_t k = start; k < total; k++) {
            uint64_t i = k % J5N_FLIGHT_N;
            char dd[96];
            j5n_disasm(sb->host_mem, d->flight.pc[i], sb->origin, sb->size, dd, sizeof dd);
            fprintf(f, "  [%6llu] pc=0x%08X  %04X  %s\n",
                    (unsigned long long)k, d->flight.pc[i], d->flight.op[i], dd);
        }
    } else {
        fprintf(f, "  (empty — the recorder is populated by the instruction-stepped run)\n");
    }
    fprintf(f, "\n");

    /* ----- threads note ----- */
    fprintf(f, "THREADS\n");
    fprintf(f, "  host thread : the single scheduler/JIT thread (R-JIT-THREAD; all JIT emit +\n");
    fprintf(f, "                execution is on one host thread under the hosted model).\n");
    fprintf(f, "  NOTE: the AmigaOS task list is an INTEGRATION-TIME hook (the real exec task\n");
    fprintf(f, "        list is owned by the AROS-side scheduler). It is NOT faked here.\n");

    fprintf(f, "\n================================================================\n");
    fprintf(f, " end of REPORT.txt — see README.txt for what to do with this bundle.\n");
    fprintf(f, "================================================================\n");
}

/* ============================ the bundle's text files ============================ */
static void write_manifest(FILE *f, j5n_fault_kind kind, int have_diverge)
{
    fprintf(f, "MANIFEST — AROS 68k JIT crash bundle (%s)\n", j5n_fault_kind_name(kind));
    fprintf(f, "================================================================\n\n");
    fprintf(f, "FILES IN THIS BUNDLE\n");
    fprintf(f, "  README.txt      Friendly plain-English overview (open this first).\n");
    fprintf(f, "  MANIFEST.txt    This file — the precise index of every file.\n");
    fprintf(f, "  REPORT.txt      The two-level human-readable crash report:\n");
    fprintf(f, "                    - the coordinate (fault kind + detail, instruction #N,\n");
    fprintf(f, "                      68k PC, in-function, host-context yes/no);\n");
    fprintf(f, "                    - the faulting 68k instruction (opcode words + disasm);\n");
    fprintf(f, "                    - 68k registers (D0-D7, A0-A7, PC, SR/CCR);\n");
    fprintf(f, "                    - host AArch64 registers (x0-x30, sp, pc, cpsr);\n");
    fprintf(f, "                    - the 68k call stack AND the native host backtrace;\n");
    fprintf(f, "                    - the flight recorder (last instructions executed).\n");
    fprintf(f, "  core.snapshot   The machine state (struct M68KState) + the FULL raw sandbox\n");
    fprintf(f, "                    memory image, reloadable for offline inspection.\n");
    fprintf(f, "  program.exe     The EXACT 68k AmigaOS hunk executable that was running.\n");
    fprintf(f, "  program.sha256  Its SHA-256 digest (so you can prove it's the same binary).\n");
    if (have_diverge)
        fprintf(f, "  diverge.txt     The exact instruction where the JIT disagreed with the\n"
                   "                    independent reference interpreter, with both states.\n");
    fprintf(f, "  REPRODUCE.txt   The deterministic replay command + the engine git commit +\n");
    fprintf(f, "                    the build config, so this reproduces on another machine.\n\n");
    fprintf(f, "THE ONE REPRODUCE STEP\n");
    fprintf(f, "  See REPRODUCE.txt — set JIT68K_RUNTO=<N> and re-run the same program; the\n");
    fprintf(f, "  engine breaks at exactly instruction #N, landing on this crash.\n");
}

static void write_reproduce(FILE *f, j5n_diag *d, j5n_fault_kind kind)
{
    fprintf(f, "REPRODUCE — deterministic replay of this crash\n");
    fprintf(f, "================================================================\n\n");
    fprintf(f, "The 68k JIT run is DETERMINISTIC (single execution, a sandboxed address space,\n");
    fprintf(f, "a deterministic stub OS, a single host thread). So the global 68k instruction\n");
    fprintf(f, "number N below pins this exact moment, and replaying to it lands here again.\n\n");
    fprintf(f, "FAULT COORDINATE\n");
    fprintf(f, "  fault kind         : %s\n", j5n_fault_kind_name(kind));
    fprintf(f, "  instruction #N     : %llu\n", (unsigned long long)(d ? d->insn_number : 0));
    fprintf(f, "  entry PC           : 0x%08X\n", d ? d->entry_pc : 0);
    fprintf(f, "  A6 library base    : 0x%08X\n\n", d ? d->a6_libbase : 0);
    fprintf(f, "REPLAY COMMAND (run-to #N)\n");
    fprintf(f, "  JIT68K_RUNTO=%llu  <the [J5n] runner>  program.exe\n",
            (unsigned long long)(d ? d->insn_number : 0));
    fprintf(f, "  -> the engine re-runs the SAME program and BREAKS at exactly instruction #N,\n");
    fprintf(f, "     landing on this crash (same PC/state). Add JIT68K_DIFF=1 to also lockstep\n");
    fprintf(f, "     against the reference interpreter and pin the first wrong instruction.\n\n");
    fprintf(f, "BUILD / PROVENANCE\n");
    fprintf(f, "  engine git commit  : %s\n", J5N_GIT_COMMIT);
    fprintf(f, "  build config       : %s\n", J5N_BUILD_CONFIG);
    fprintf(f, "  the program bytes + its SHA-256 are in program.exe / program.sha256.\n");
}

static void write_readme(FILE *f, j5n_fault_kind kind, int have_diverge)
{
    fprintf(f, "What is this?\n");
    fprintf(f, "------------\n");
    fprintf(f, "This is a crash diagnostic bundle from the AROS 68k JIT. A 68k program faulted\n");
    fprintf(f, "while running; this archive has everything needed to reproduce and fix it.\n\n");
    fprintf(f, "What should I do?\n");
    fprintf(f, "-----------------\n");
    fprintf(f, "Send this entire .tar.gz file to the developer. That's all you need to do.\n\n");
    fprintf(f, "What's inside?\n");
    fprintf(f, "--------------\n");
    fprintf(f, "  README.txt      This file — the friendly overview.\n");
    fprintf(f, "  MANIFEST.txt    The precise index of every file (the detailed version of this).\n");
    fprintf(f, "  REPORT.txt      The human-readable crash report: the registers and the call\n");
    fprintf(f, "                  stacks — what went wrong and where.\n");
    fprintf(f, "  core.snapshot   The machine state plus a copy of the program's memory.\n");
    fprintf(f, "  program.exe     The exact program that crashed (program.sha256 is its\n");
    fprintf(f, "                  fingerprint, to prove it's the same one).\n");
    if (have_diverge)
        fprintf(f, "  diverge.txt     Where the JIT disagreed with the reference interpreter\n"
                   "                  (this usually means the JIT itself has a bug).\n");
    fprintf(f, "  REPRODUCE.txt   The exact command to replay the crash on another machine.\n\n");
    fprintf(f, "For the developer\n");
    fprintf(f, "-----------------\n");
    fprintf(f, "The workflow this bundle is built for:\n");
    fprintf(f, "  1. The differential (lockstep) mode pins the EXACT wrong instruction — run the\n");
    fprintf(f, "     same program with JIT68K_DIFF=1 (see diverge.txt if it's already here).\n");
    fprintf(f, "  2. The fault coordinate (instruction #N) + core.snapshot reproduce that exact\n");
    fprintf(f, "     moment deterministically: JIT68K_RUNTO=<N> re-runs and stops right there\n");
    fprintf(f, "     (see REPRODUCE.txt).\n");
    fprintf(f, "  3. The two-level register/stack dump in REPORT.txt — the 68k guest state AND\n");
    fprintf(f, "     the host AArch64 state — tells you whether this is a bug in the 68k program\n");
    fprintf(f, "     or a bug in the JIT translating it.\n\n");
    fprintf(f, "(Fault kind: %s.)\n", j5n_fault_kind_name(kind));
}

static void write_diverge(FILE *f, j5n_diag *d)
{
    fprintf(f, "DIVERGENCE — the JIT disagreed with the reference interpreter\n");
    fprintf(f, "================================================================\n\n");
    fprintf(f, "The differential (lockstep) mode ran the independent from-scratch interpreter\n");
    fprintf(f, "(the oracle, OURS, no Emu68) in lockstep with the JIT. This is the FIRST 68k\n");
    fprintf(f, "instruction where their state diverged — i.e. the JIT mistranslated it.\n\n");
    fprintf(f, "  instruction #N : %llu\n", (unsigned long long)d->insn_number);
    fprintf(f, "  68k PC         : 0x%08X\n", d->diverge_pc);
    fprintf(f, "  opcode word    : 0x%04X\n", d->diverge_op);
    {
        char dis[128];
        j5n_disasm(d->sb->host_mem, d->diverge_pc, d->sb->origin, d->sb->size, dis, sizeof dis);
        fprintf(f, "  disassembly    : %s\n", dis);
    }
    {
        uint32_t pd=0; const j5n_sym *s = j5n_symbols_lookup(d->symtab, d->diverge_pc, 0x10000, &pd);
        if (s) fprintf(f, "  in function    : %s + 0x%X\n", s->name, pd);
    }
    fprintf(f, "  what diverged  : %s\n\n", d->diverge_what);
    fprintf(f, "STATE SIDE BY SIDE (after executing the diverging instruction)\n");
    fprintf(f, "  reg     JIT          ORACLE       %s\n", "");
    for (int i = 0; i < 8; i++)
        fprintf(f, "  D%d   0x%08X   0x%08X   %s\n", i, d->diverge_jit.d[i], d->diverge_ref.d[i],
                d->diverge_jit.d[i] != d->diverge_ref.d[i] ? "<<< DIFFERS" : "");
    for (int i = 0; i < 8; i++)
        fprintf(f, "  A%d   0x%08X   0x%08X   %s\n", i, d->diverge_jit.a[i], d->diverge_ref.a[i],
                d->diverge_jit.a[i] != d->diverge_ref.a[i] ? "<<< DIFFERS" : "");
    fprintf(f, "  CCR  0x%08X   0x%08X   %s\n", d->diverge_jit.ccr, d->diverge_ref.ccr,
            d->diverge_jit.ccr != d->diverge_ref.ccr ? "<<< DIFFERS" : "");
    fprintf(f, "  PC   0x%08X   0x%08X   %s\n", d->diverge_jit.pc, d->diverge_ref.pc,
            d->diverge_jit.pc != d->diverge_ref.pc ? "<<< DIFFERS" : "");
}

/* ============================ core.snapshot (binary, reloadable) ============================
 * Layout: a fixed magic header + struct j5d_m68k_state + the raw sandbox image. */
#define J5N_SNAP_MAGIC 0x4A354E53u   /* 'J5NS' */
static int write_snapshot(const char *path, const struct j5d_m68k_state *st, j5d_sandbox *sb)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    uint32_t hdr[4] = { J5N_SNAP_MAGIC, sb->origin, sb->size, (uint32_t)sizeof(*st) };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(st, sizeof(*st), 1, f);
    fwrite(sb->host_mem, 1, sb->size, f);
    fclose(f);
    return 0;
}

int j5n_snapshot_load(const char *path, struct j5d_m68k_state *st,
                      uint8_t **image, size_t *image_len, uint32_t *origin)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    uint32_t hdr[4];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != J5N_SNAP_MAGIC) { fclose(f); return 1; }
    uint32_t sz = hdr[2];
    if (fread(st, sizeof(*st), 1, f) != 1) { fclose(f); return 1; }
    uint8_t *img = malloc(sz);
    if (!img) { fclose(f); return 1; }
    if (fread(img, 1, sz, f) != sz) { free(img); fclose(f); return 1; }
    fclose(f);
    *image = img; *image_len = sz; if (origin) *origin = hdr[1];
    return 0;
}

/* ============================ helpers: dir + tar + banner ============================ */
static int write_bytes(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w != len;
}

static void utc_stamp(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm g;
    gmtime_r(&t, &g);
    strftime(out, n, "%Y%m%dT%H%M%SZ", &g);
}

/* The LOUD banner — printed to stderr AND stdout so it's never missed. */
static void loud_banner(const char *archive_abs, j5n_fault_kind kind)
{
    const char *bar = "============================================================================";
    fprintf(stderr, "\n%s\n", bar);
    fprintf(stderr, "  !!  AROS 68k JIT FAULT (%s)  --  a crash bundle was written.\n",
            j5n_fault_kind_name(kind));
    fprintf(stderr, "  !!  %s\n", archive_abs);
    fprintf(stderr, "  !!  SEND THIS FILE to the developer.\n");
    fprintf(stderr, "  !!  (open README.txt inside if you're not sure what this is.)\n");
    fprintf(stderr, "%s\n\n", bar);
    fflush(stderr);
    printf("\n%s\n", bar);
    printf("  !!  AROS 68k JIT FAULT (%s)  —  a crash bundle was written.\n",
           j5n_fault_kind_name(kind));
    printf("  !!  %s\n", archive_abs);
    printf("  !!  SEND THIS FILE to the developer.\n");
    printf("  !!  (open README.txt inside if you're not sure what this is.)\n");
    printf("%s\n\n", bar);
    fflush(stdout);
}

/* =================================== THE FUNNEL =================================== */
const char *j5d_fault(j5n_fault_kind kind, const char *detail,
                      const struct j5d_m68k_state *st, j5d_sandbox *sb,
                      const j5n_hostregs *host)
{
    j5n_diag *d = g_engine_diag;

    /* Resolve the crash dir: explicit -> $JIT68K_CRASH_DIR -> <cwd>/crash. */
    const char *cdir = (d && d->crash_dir) ? d->crash_dir
                     : (getenv("JIT68K_CRASH_DIR") ? getenv("JIT68K_CRASH_DIR") : "crash");
    mkdir(cdir, 0755);

    char stamp[32]; utc_stamp(stamp, sizeof stamp);
    char bundledir[1024];
    snprintf(bundledir, sizeof bundledir, "%s/jit68k-crash-%s-%s",
             cdir, stamp, j5n_fault_kind_name(kind));
    /* de-collide if two faults share a UTC second. */
    {
        struct stat sbuf; int seq = 0; char tryd[1100];
        snprintf(tryd, sizeof tryd, "%s", bundledir);
        while (stat(tryd, &sbuf) == 0 && seq < 1000)
            snprintf(tryd, sizeof tryd, "%s-%d", bundledir, ++seq);
        snprintf(bundledir, sizeof bundledir, "%s", tryd);
    }
    if (mkdir(bundledir, 0755) != 0) {
        fprintf(stderr, "[J5n] j5d_fault: cannot create bundle dir %s: %s\n",
                bundledir, strerror(errno));
        return NULL;
    }

    int have_diverge = (d && d->diverged);
    char path[1200];

    /* REPORT.txt */
    snprintf(path, sizeof path, "%s/REPORT.txt", bundledir);
    { FILE *f = fopen(path, "w"); if (f) { write_report(f, kind, detail, st, sb, host, d); fclose(f); } }

    /* README.txt + MANIFEST.txt + REPRODUCE.txt */
    snprintf(path, sizeof path, "%s/README.txt", bundledir);
    { FILE *f = fopen(path, "w"); if (f) { write_readme(f, kind, have_diverge); fclose(f); } }
    snprintf(path, sizeof path, "%s/MANIFEST.txt", bundledir);
    { FILE *f = fopen(path, "w"); if (f) { write_manifest(f, kind, have_diverge); fclose(f); } }
    snprintf(path, sizeof path, "%s/REPRODUCE.txt", bundledir);
    { FILE *f = fopen(path, "w"); if (f) { write_reproduce(f, d, kind); fclose(f); } }

    /* core.snapshot */
    snprintf(path, sizeof path, "%s/core.snapshot", bundledir);
    write_snapshot(path, st, sb);

    /* program.exe + program.sha256 */
    if (d && d->prog && d->prog_len) {
        snprintf(path, sizeof path, "%s/program.exe", bundledir);
        write_bytes(path, d->prog, d->prog_len);
        char hex[65]; sha256(d->prog, d->prog_len, hex);
        char line[80]; snprintf(line, sizeof line, "%s  program.exe\n", hex);
        snprintf(path, sizeof path, "%s/program.sha256", bundledir);
        write_bytes(path, line, strlen(line));
    }

    /* diverge.txt (differential mode only) */
    if (have_diverge) {
        snprintf(path, sizeof path, "%s/diverge.txt", bundledir);
        FILE *f = fopen(path, "w"); if (f) { write_diverge(f, d); fclose(f); }
    }

    /* tar -czf the bundle. */
    char archive[1300];
    snprintf(archive, sizeof archive, "%s.tar.gz", bundledir);
    {
        /* tar from the crash dir so the archive holds the bundle dir by basename. */
        char base[1024]; const char *slash = strrchr(bundledir, '/');
        snprintf(base, sizeof base, "%s", slash ? slash + 1 : bundledir);
        char cmd[3000];
        snprintf(cmd, sizeof cmd, "tar -czf '%s' -C '%s' '%s' 2>/dev/null", archive, cdir, base);
        int rc = system(cmd);
        (void)rc;
    }

    /* absolute path for the banner. */
    char abs[1400];
    if (archive[0] == '/') snprintf(abs, sizeof abs, "%s", archive);
    else {
        char cwd[1024];
        if (getcwd(cwd, sizeof cwd)) snprintf(abs, sizeof abs, "%s/%s", cwd, archive);
        else snprintf(abs, sizeof abs, "%s", archive);
    }

    if (d) {
        snprintf(d->last_bundle, sizeof d->last_bundle, "%s", abs);
        d->bundles_written++;
    }
    if (!d || !d->quiet_banner)
        loud_banner(abs, kind);
    return d ? d->last_bundle : NULL;
}

/* ============================ the host-signal safety net ============================ */
static j5n_diag *g_sig_diag = NULL;
static const struct j5d_m68k_state *g_sig_state = NULL;
static j5d_sandbox *g_sig_sb = NULL;
static stack_t g_altstack;
static int g_alt_installed = 0;

void j5n_signal_set_context(const struct j5d_m68k_state *st, j5d_sandbox *sb)
{
    g_sig_state = st; g_sig_sb = sb;
}

#if defined(__APPLE__)
#include <sys/ucontext.h>
#endif

static void fill_hostregs_from_uc(j5n_hostregs *h, void *ucv)
{
    memset(h, 0, sizeof(*h));
#if defined(__APPLE__)
    ucontext_t *uc = (ucontext_t *)ucv;
    if (uc && uc->uc_mcontext) {
        for (int i = 0; i < 29; i++) h->x[i] = uc->uc_mcontext->__ss.__x[i];
        h->x[29] = (uint64_t)uc->uc_mcontext->__ss.__fp;
        h->x[30] = (uint64_t)uc->uc_mcontext->__ss.__lr;
        h->sp    = (uint64_t)uc->uc_mcontext->__ss.__sp;
        h->pc    = (uint64_t)uc->uc_mcontext->__ss.__pc;
        h->cpsr  = (uint32_t)uc->uc_mcontext->__ss.__cpsr;
        h->have  = 1;
    }
#else
    (void)ucv;
#endif
}

static void host_signal_handler(int sig, siginfo_t *info, void *ucv)
{
    j5n_fault_kind kind;
    char detail[160];
    switch (sig) {
        case SIGSEGV: kind = J5N_FAULT_HOST_SIGNAL;
            snprintf(detail, sizeof detail, "host SIGSEGV (invalid memory access at %p) in translated code",
                     info ? info->si_addr : NULL); break;
        case SIGBUS:  kind = J5N_FAULT_HOST_SIGNAL;
            snprintf(detail, sizeof detail, "host SIGBUS (bus error at %p) in translated code",
                     info ? info->si_addr : NULL); break;
        case SIGILL:  kind = J5N_FAULT_ILLEGAL;
            snprintf(detail, sizeof detail, "host SIGILL (illegal host instruction at %p)",
                     info ? info->si_addr : NULL); break;
        case SIGFPE:  kind = J5N_FAULT_DIVZERO;
            snprintf(detail, sizeof detail, "host SIGFPE (arithmetic exception at %p)",
                     info ? info->si_addr : NULL); break;
        default:      kind = J5N_FAULT_HOST_SIGNAL;
            snprintf(detail, sizeof detail, "host signal %d in translated code", sig); break;
    }
    j5n_hostregs h; fill_hostregs_from_uc(&h, ucv);

    /* Recover the current 68k state the engine registered before entering the block. If we
     * have none (signal outside a run), synthesize a minimal one so the bundle still writes. */
    static struct j5d_m68k_state synth;
    static j5d_sandbox synth_sb;
    const struct j5d_m68k_state *st = g_sig_state;
    j5d_sandbox *sb = g_sig_sb;
    if (!st || !sb) {
        memset(&synth, 0, sizeof synth);
        memset(&synth_sb, 0, sizeof synth_sb);
        st = &synth; sb = &synth_sb;
    }
    (void)g_sig_diag;
    j5d_fault(kind, detail, st, sb, &h);

    /* We have bundled the fault — do not loop. Restore the default handler and re-raise so
     * the process dies with the original signal disposition (the host crash is now diagnosed,
     * not silent). */
    signal(sig, SIG_DFL);
    raise(sig);
}

void j5n_signal_install(j5n_diag *d)
{
    g_sig_diag = d;
    if (!g_alt_installed) {
        static char altmem[SIGSTKSZ < 65536 ? 65536 : SIGSTKSZ];
        g_altstack.ss_sp = altmem;
        g_altstack.ss_size = sizeof altmem;
        g_altstack.ss_flags = 0;
        sigaltstack(&g_altstack, NULL);
        g_alt_installed = 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = host_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}

void j5n_signal_remove(void)
{
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS,  SIG_DFL);
    signal(SIGILL,  SIG_DFL);
    signal(SIGFPE,  SIG_DFL);
    g_sig_diag = NULL; g_sig_state = NULL; g_sig_sb = NULL;
}

/* ============================ the per-instruction step hook ============================
 * Called by the interp oracle at its loop top (the instruction-precise stepper). It owns:
 *   - the deterministic global instruction counter (insn_number, 0-based: #0 is entry);
 *   - the flight recorder push;
 *   - replay-to-N: STOP exactly when insn_number == runto_n;
 *   - diff mode: the JIT runs separately; here the oracle just advances and records. The
 *     actual lockstep COMPARE happens in the engine at block boundaries (see j5d_engine.c),
 *     which calls back the oracle to localize the diverging instruction. So in pure-oracle
 *     stepping the step hook records the trail; the engine owns the compare + the trap.
 * Returns nonzero to STOP the run. */
int j5n_diag_step(j5n_diag *d, const struct j5d_m68k_state *st, j5d_sandbox *sb,
                  uint32_t pc, uint16_t op)
{
    (void)st; (void)sb;
    if (!d) return 0;
    j5n_flight_push(d, pc, op);
    /* replay-to-N: break BEFORE executing instruction #N (we have already counted up to it). */
    if (d->runto_enabled && d->insn_number == d->runto_n) {
        d->runto_hit = 1;
        d->runto_pc = pc;
        return 1;          /* stop the oracle run here */
    }
    d->insn_number++;
    return 0;
}
