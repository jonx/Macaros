/* refcpu.c — the small INDEPENDENT 68k reference interpreter (OURS, AROS-licensed).
 *
 * Clean-room / OURS, from the Motorola 68000 ISA only. Implements refcpu.h: the
 * oracle the runner uses to compute the EXPECTED result of programs that need
 * [J5c]-level decoder coverage. Executes over the real [J4] sandbox (memory effects
 * are genuine) and routes jsr-through-vector library calls to a dispatch callback.
 * It is NOT the JIT and never stands in for a JIT pass; the runner reports JIT-vs-
 * reference status explicitly.
 *
 * Decodes exactly the opcodes the four apps68k programs use (see refcpu.h). All
 * fetches are big-endian (the sandbox is 68k big-endian).
 */
#include "refcpu.h"
#include "j3_jit68k.h"           /* struct M68KState + the cpu.h vector stride */

#include <string.h>
#include <stdio.h>

#define STEP_CAP 1000000u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

int refcpu_run(j4_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
               struct M68KState *regs, uint32_t *exit_d0,
               refcpu_libcall_fn libcall, void *user,
               char *errbuf, unsigned errlen)
{
#define RFAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 1; } while (0)

    memset(regs, 0, sizeof(*regs));
    regs->a[6] = a6_libbase;            /* A6 = library base (the AmigaOS convention) */
    uint32_t pc = entry_pc;
    uint32_t steps = 0;

    for (;;) {
        if (++steps > STEP_CAP) RFAIL("step cap exceeded (runaway / mis-decode)");
        const uint8_t *ip = j4_sandbox_host(sb, pc);
        uint16_t op = rd16(ip);
        regs->pc = pc;

        /* rts */
        if (op == 0x4E75u) { *exit_d0 = regs->d[0]; return 0; }
        if (op == 0x4E71u) { pc += 2; continue; }            /* nop (vasm tail pad) */

        /* jsr d16(A6) : 0x4EAE, then a signed 16-bit displacement. */
        if (op == 0x4EAEu) {
            int16_t d16 = (int16_t)rd16(ip + 2);
            uint32_t target = a6_libbase + (uint32_t)(int32_t)d16;   /* libbase + d16 */
            /* Recover the LVO via the cpu.h negative-offset rule: n = (lib - pc)/6. */
            if (target > a6_libbase) RFAIL("jsr(A6) target above libbase");
            uint32_t delta = a6_libbase - target;
            if (delta % J3_M68K_LIB_VECTSIZE) RFAIL("jsr(A6) not on a 6-byte vector");
            int lvo = (int)(delta / J3_M68K_LIB_VECTSIZE);
            if (!libcall) RFAIL("library call but no dispatch callback");
            if (libcall(lvo, regs, user, errbuf, errlen)) return 1;   /* errbuf set */
            pc += 4;
            continue;
        }

        /* moveq #imm8,Dn : 0111 ddd0 iiiiiiii */
        if ((op & 0xF100u) == 0x7000u) {
            unsigned dn = (op >> 9) & 7u;
            regs->d[dn] = (uint32_t)(int32_t)(int8_t)(op & 0xFFu);
            pc += 2; continue;
        }

        /* move.l #imm32,Dn : 0010 ddd0 00 111 100 + imm32 */
        if ((op & 0xF1FFu) == 0x203Cu) {
            unsigned dn = (op >> 9) & 7u;
            regs->d[dn] = rd32(ip + 2);
            pc += 6; continue;
        }
        /* movea.l #imm32,An : 0010 aaa0 01 111 100 + imm32 */
        if ((op & 0xF1FFu) == 0x207Cu) {
            unsigned an = (op >> 9) & 7u;
            regs->a[an] = rd32(ip + 2);
            pc += 6; continue;
        }

        /* move.l Dm,Dn (reg->reg) : 0010 ddd0 00 000 mmm */
        if ((op & 0xF1F8u) == 0x2000u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            regs->d[dn] = regs->d[dm];
            pc += 2; continue;
        }
        /* movea.l Dn,An : 0010 aaa0 01 000 nnn */
        if ((op & 0xF1F8u) == 0x2040u) {
            unsigned an = (op >> 9) & 7u, dn = op & 7u;
            regs->a[an] = regs->d[dn];
            pc += 2; continue;
        }
        /* movea.l An,Am : 0010 aaa0 01 001 nnn */
        if ((op & 0xF1F8u) == 0x2048u) {
            unsigned am = (op >> 9) & 7u, an = op & 7u;
            regs->a[am] = regs->a[an];
            pc += 2; continue;
        }

        /* lea abs.l,An : 0100 aaa1 11 111 001 + abs32  (vasm emits 41F9 for lea label,a0) */
        if ((op & 0xF1FFu) == 0x41F9u) {
            unsigned an = (op >> 9) & 7u;
            regs->a[an] = rd32(ip + 2);    /* abs32 was relocated by the loader */
            pc += 6; continue;
        }

        /* add.l Dm,Dn : 1101 ddd0 10 000 mmm */
        if ((op & 0xF1F8u) == 0xD080u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            regs->d[dn] += regs->d[dm];
            pc += 2; continue;
        }
        /* add.l (An)+,Dn : 1101 ddd0 10 011 aaa  (post-increment) */
        if ((op & 0xF1F8u) == 0xD098u) {
            unsigned dn = (op >> 9) & 7u, an = op & 7u;
            uint32_t addr = regs->a[an];
            if (addr < sb->sandbox_origin ||
                (uint64_t)addr + 4u > (uint64_t)sb->sandbox_origin + sb->size)
                RFAIL("add.l (An)+ address out of sandbox");
            regs->d[dn] += rd32(j4_sandbox_host(sb, addr));
            regs->a[an]  = addr + 4u;       /* post-increment by 4 (.L) */
            pc += 2; continue;
        }

        /* addq.l #imm,Dn : 0101 qqq0 10 000 ddd */
        if ((op & 0xF1F8u) == 0x5080u) {
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, dn = op & 7u;
            regs->d[dn] += imm;
            pc += 2; continue;
        }
        /* subq.l #imm,Dn : 0101 qqq1 10 000 ddd  (sets Z for the bne) */
        if ((op & 0xF1F8u) == 0x5180u) {
            unsigned q = (op >> 9) & 7u, imm = (q == 0) ? 8u : q, dn = op & 7u;
            uint32_t res = regs->d[dn] - imm;
            regs->d[dn] = res;
            regs->ccr = (regs->ccr & ~0x04u) | (res == 0 ? 0x04u : 0u);   /* Z */
            pc += 2; continue;
        }

        /* cmp.l Dm,Dn : 1011 ddd0 10 000 mmm  -> sets Z = (Dn == Dm) */
        if ((op & 0xF1F8u) == 0xB080u) {
            unsigned dn = (op >> 9) & 7u, dm = op & 7u;
            uint32_t res = regs->d[dn] - regs->d[dm];
            regs->ccr = (regs->ccr & ~0x04u) | (res == 0 ? 0x04u : 0u);   /* Z */
            pc += 2; continue;
        }

        /* bne.s disp8 : 0110 0110 dddddddd  (Z==0 -> taken) */
        if ((op & 0xFF00u) == 0x6600u) {
            int32_t disp = (int8_t)(op & 0xFFu);
            if (disp == 0) RFAIL("bne.W/.L not in reference subset");
            if (!(regs->ccr & 0x04u)) pc = (uint32_t)((int64_t)pc + 2 + disp);  /* taken */
            else pc += 2;
            continue;
        }

        RFAIL("refcpu: out-of-subset opcode");
    }
#undef RFAIL
}
