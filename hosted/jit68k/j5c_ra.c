/* j5c_ra.c — [J5c] HOOK 3: OUR register allocator, implementing the RA_* interface the
 * REAL Emu68 decoders call (emu68/RegisterAllocator.h), memory-backed instead of using
 * EL0 system registers (OURS, AROS-licensed).
 *
 * LICENCE BOUNDARY: THIS FILE is OURS. It implements the interface DECLARED in the
 * quarantined emu68/RegisterAllocator.h (a declarations-only header — declarations are
 * facts, not expression). No Emu68 function body is copied: each RA_* body here is
 * authored from the documented contract + the [J5a] finding. In particular the SR/CTX
 * functions do NOT use `mrs/msr TPIDR_EL0/TPIDRRO_EL0` (Emu68's bare-metal convention,
 * the third [J5a] coupling) — they keep the CCR in an ARM scratch register loaded from /
 * stored to struct j5c_m68k_state (x1, [J5g]) in MEMORY.
 *
 * ===================== THE FIXED m68k->ARM REGISTER MAP (Emu68 ABI) =================
 * The decoders + EA assume Dn/An are ALWAYS mapped to fixed ARM registers (RA_Map* just
 * returns the mapping; there is no spill for architectural regs). We reuse the SAME map
 * as Emu68 (it is an ABI fact baked into A64.h's REG_D0.. constants), so the vendored
 * decoders' RA_MapM68kRegister(n) returns w19.. for D0.. and w13../w27.. for A0..:
 *   D0..D7 -> w19..w26 ; A0..A4 -> w13..w17 ; A5..A7 -> w27..w29 ; PC -> w18.
 * j5c_build.c's prologue loads these from the state struct and the epilogue stores them
 * back, so memory-backing the architectural regs needs nothing here.
 *
 * Scratch (temporary) ARM registers: the allocator hands out w4..w11 (bits 0..3 are
 * reserved). This matches Emu68's own __int_arm_alloc_reg (`~(register_pool | 15)` =>
 * first free bit >= 4).
 *
 * THE STATE POINTER IS IN x1, NOT x0 ([J5g] FIX — a FOURTH bare-metal coupling).
 * Emu68's decoders treat w0 as a hardcoded 1-shot SCRATCH for flag extraction — e.g.
 * EMIT_BTST / EMIT_ASx do `cset(0, cond); bfi(cc, 0, SRB_Z, 1)` (M68k_LINE0.c:1748,
 * M68k_LINEE.c:869, ...) — because bare-metal the CTX lives in a system register
 * (TPIDRRO_EL0), so w0 is genuinely free. The [J5c]/[J5d] re-host overloaded w0 as the
 * MEMORY-backed state pointer, which those `cset(0,...)` sites silently clobbered (the
 * register/ALU corpus never hit them; the [J5g] demanding program's btst does). The fix:
 * keep the state pointer in x1 (the decoders NEVER hardcode w1/w2/w3 — verified), so w0
 * is free exactly as Emu68 expects. RA_GetCC/RA_GetCTX + the engine prologue/epilogue use
 * x1 as the state base; the EA helpers use x12 (base-adjust) + x2/x3 (scratch).
 *
 * ============================= THE CCR (the [J5a] coupling, re-hosted) ==============
 * Emu68's RA_GetCC does `mrs reg, TPIDR_EL0` to load the CCR and RA_StoreCC does `msr`.
 * Ours instead does `ldr w_cc,[x1,#J5C_OFF_CCR]` / `str w_cc,[x1,#J5C_OFF_CCR]`. The
 * decoders use the CCR register in Emu68's swapped representation (C/V swapped vs the 68k
 * SR bit order in some paths — documented in A64.h). We store/restore the byte verbatim:
 * the INTERPRETER oracle (j5c_interp.c) and the test assert the architectural registers
 * byte-exact (the real proof) and check the Z/N CCR bits, which are layout-stable.
 * ===============================================================================
 */
#define J5C_STATE_X  1u    /* x1 = struct j5c_m68k_state* for the whole block */
#include <stdint.h>
#include "emu68/RegisterAllocator.h"
#include "emu68/A64.h"            /* encoders: ldr_offset/str_offset/mov_reg; REG_* map  */
#include "j5c_jit68k.h"           /* J5C_OFF_CCR (the memory-backed CCR offset)          */

/* The fixed m68k(0..15) -> ARM map, identical to Emu68's _reg_map_m68k_to_arm. */
static const uint8_t _map[16] = {
    REG_D0, REG_D1, REG_D2, REG_D3, REG_D4, REG_D5, REG_D6, REG_D7,
    REG_A0, REG_A1, REG_A2, REG_A3, REG_A4, REG_A5, REG_A6, REG_A7
};

/* Scratch allocator over w4..w11 (bits 0..3 reserved; x0 = state pointer). */
static uint16_t register_pool = 0;
static uint16_t changed_mask  = 0;

/* =========================== [J5e] block-scoped liveness ==========================
 * The fixed Emu68 map keeps Dn/An permanently in host regs across the block (w19..w26 /
 * w13..w17,w27..w29) — there is no per-op spill of the architectural file inside the
 * decoders. The engine's prologue/epilogue, however, loaded ALL 16 regs and stored ALL
 * 16 back unconditionally. [J5e] makes that block-scoped: load only the registers the
 * block READS before writing (live-in), store back only the registers the block WRITES
 * (dirty). These three masks (bit i = D i for i<8, A(i-8) for i>=8) are accumulated as
 * the REAL decoders call RA_MapM68kRegister (read) / RA_MapM68kRegisterForWrite /
 * RA_SetDirtyM68kRegister (write), then read by the engine to emit a minimal frame.
 *
 * The read-BEFORE-write rule is the load-bearing correctness point: a register read for
 * the FIRST time after it was already written in this block is NOT live-in (its value is
 * already in the host reg), and a PARTIAL write (.W/.B without sign-extend: bfi/bfxil)
 * goes through RA_MapM68kRegister FIRST in the decoders, so it is correctly marked
 * live-in (the upper bits must be preserved). A FULL write (.L mov, or sign-extended
 * .W/.B) goes through RA_MapM68kRegisterForWrite with no prior read, so it is NOT
 * live-in — exactly the moveq #imm / move.l #imm,Dn case the prologue can skip loading. */
static uint16_t live_mask    = 0;   /* read before written: must be loaded in prologue   */
static uint16_t dirty_mask   = 0;   /* written by the block: must be stored in epilogue   */
static uint16_t written_mask = 0;   /* written SO FAR (internal: read-before-write gate)  */

/* [J5o] FPU allocator + FP-register liveness + FPCR/FPSR scratch state. Declared HERE (up
 * front) because j5c_ra_reset() — which precedes the FPU function bodies below — resets
 * them. The real getter/mapper/FPCR/FPSR bodies are in the [J5o] FPU RA section near the
 * bottom of this file. */
static uint8_t  fpu_allocstate  = 0; /* d2..d7 scratch pool (bit i = di busy)              */
static uint16_t fp_live_mask    = 0; /* FP regs read before written: load in prologue      */
static uint16_t fp_dirty_mask   = 0; /* FP regs written by the block: store in epilogue     */
static uint16_t fp_written_mask = 0; /* written SO FAR (read-before-write gate, internal)   */
static uint8_t  reg_FPCR = 0xff;     /* lazily-loaded scratch mirroring st->fpcr            */
static uint8_t  mod_FPCR = 0;
static uint8_t  reg_FPSR = 0xff;     /* lazily-loaded scratch mirroring st->fpsr            */
static uint8_t  mod_FPSR = 0;

static inline void ra_note_read(uint8_t m68k_reg)
{
    uint16_t bit = (uint16_t)(1u << (m68k_reg & 15));
    if (!(written_mask & bit)) live_mask |= bit;   /* read before any write => live-in */
}
static inline void ra_note_write(uint8_t m68k_reg)
{
    uint16_t bit = (uint16_t)(1u << (m68k_reg & 15));
    dirty_mask   |= bit;
    written_mask |= bit;
}

/* The engine reads these after driving the decoders for a block to size its frame. */
void j5c_ra_get_masks(uint16_t *live, uint16_t *dirty)
{
    if (live)  *live  = live_mask;
    if (dirty) *dirty = dirty_mask;
}

static uint8_t alloc_scratch(void)
{
    int reg = __builtin_ctz(~(register_pool | 15));   /* first free bit >= 4 */
    if (reg < 12) { register_pool |= 1u << reg; changed_mask |= 1u << reg; return (uint8_t)reg; }
    return 0xff;
}

/* ---- architectural register mapping (always-mapped; no spill) ----------------------
 * The mapping is the fixed Emu68 ABI (no renaming) — but [J5e] records the read/write so
 * the engine can load only live-in regs and store only dirty regs (see liveness above). */
uint8_t RA_MapM68kRegister(uint32_t **s, uint8_t m68k_reg)          { (void)s; ra_note_read(m68k_reg);  return _map[m68k_reg & 15]; }
uint8_t RA_MapM68kRegisterForWrite(uint32_t **s, uint8_t m68k_reg)  { (void)s; ra_note_write(m68k_reg); return _map[m68k_reg & 15]; }
void    RA_SetDirtyM68kRegister(uint32_t **s, uint8_t m68k_reg)     { (void)s; ra_note_write(m68k_reg); }
void    RA_TouchM68kRegister(uint32_t **s, uint8_t m68k_reg)        { (void)s; (void)m68k_reg; }
void    RA_InsertM68kRegister(uint32_t **s, uint8_t m68k_reg)       { (void)s; (void)m68k_reg; }
void    RA_RemoveM68kRegister(uint32_t **s, uint8_t m68k_reg)       { (void)s; (void)m68k_reg; }
void    RA_DiscardM68kRegister(uint32_t **s, uint8_t m68k_reg)      { (void)s; (void)m68k_reg; }
void    RA_UnmapM68kRegister(uint32_t **s, uint8_t m68k_reg)        { (void)s; (void)m68k_reg; }
void    RA_AssignM68kRegister(uint32_t **s, uint8_t m, uint8_t a)   { (void)s; (void)m; (void)a; }
void    RA_FlushM68kRegs(uint32_t **s)                              { (void)s; }
void    RA_StoreDirtyM68kRegs(uint32_t **s)                         { (void)s; }
uint8_t RA_GetMappedARMRegister(uint8_t m68k_reg)                   { return _map[m68k_reg & 15]; }

int RA_IsM68kRegister(uint8_t arm_reg)
{
    if (arm_reg >= 32) return 0;
    for (int i = 0; i < 16; i++) if (_map[i] == arm_reg) return 1;
    return 0;
}
uint8_t RA_IsARMRegisterMapped(uint8_t arm_reg) { return (uint8_t)RA_IsM68kRegister(arm_reg); }

/* Make a discardable copy of a m68k register into a fresh scratch reg. */
uint8_t RA_CopyFromM68kRegister(uint32_t **s, uint8_t m68k_reg)
{
    uint8_t r = alloc_scratch();
    ra_note_read(m68k_reg);          /* reads the source m68k reg => live-in if unwritten */
    **s = mov_reg(r, _map[m68k_reg & 15]);
    (*s)++;
    return r;
}

/* ---- scratch ARM register pool ----------------------------------------------------- */
uint8_t RA_AllocARMRegister(uint32_t **s) { (void)s; return alloc_scratch(); }
void    RA_FreeARMRegister(uint32_t **s, uint8_t arm_reg) { (void)s; if (arm_reg <= 11) register_pool &= ~(1u << arm_reg); }
uint16_t RA_GetTempAllocMask(void) { return register_pool; }
uint16_t RA_GetChangedMask(void)   { return changed_mask; }
void     RA_ClearChangedMask(void) { changed_mask = 0; }

/* ===================================================================================
 * HOOK 3 — the CCR, MEMORY-BACKED (NOT TPIDR_EL0).  reg_CC holds a scratch register that
 * mirrors struct j5c_m68k_state.ccr; loaded lazily on first use, stored if modified.
 * x1 holds the state pointer for the whole block ([J5g]); the offset load works.
 * =================================================================================== */
static uint8_t reg_CC = 0xff;
static uint8_t mod_CC = 0;

uint8_t RA_GetCC(uint32_t **ptr)
{
    if (reg_CC == 0xff) {
        reg_CC = alloc_scratch();
        /* ldr w_cc, [x0, #CCR]  — load the CCR from the state struct (NOT mrs TPIDR). */
        **ptr = ldr_offset(J5C_STATE_X /*x1 = state*/, reg_CC, J5C_OFF_CCR);
        (*ptr)++;
        mod_CC = 0;
    }
    return reg_CC;
}

uint8_t RA_ModifyCC(uint32_t **ptr) { uint8_t cc = RA_GetCC(ptr); mod_CC = 1; return cc; }

void RA_StoreCC(uint32_t **ptr)
{
    if (reg_CC != 0xff && mod_CC) {
        /* str w_cc, [x0, #CCR]  — store the CCR back to the state struct (NOT msr). */
        **ptr = str_offset(J5C_STATE_X, reg_CC, J5C_OFF_CCR);
        (*ptr)++;
    }
}

void RA_FlushCC(uint32_t **ptr)
{
    if (reg_CC != 0xff) {
        if (mod_CC) { **ptr = str_offset(J5C_STATE_X, reg_CC, J5C_OFF_CCR); (*ptr)++; }
        RA_FreeARMRegister(ptr, reg_CC);
    }
    reg_CC = 0xff;
    mod_CC = 0;
}

int RA_IsCCLoaded(void)   { return reg_CC != 0xff; }
int RA_IsCCModified(void) { return mod_CC != 0; }

/* Reset CCR allocator state at the start of each block emit. */
void j5c_ra_reset(void)
{
    register_pool = 0;
    changed_mask  = 0;
    reg_CC = 0xff;
    mod_CC = 0;
    live_mask    = 0;   /* [J5e] block-scoped liveness, reset per block */
    dirty_mask   = 0;
    written_mask = 0;
    /* [J5o] FPU allocator + FP liveness + FPCR/FPSR scratch, reset per block. */
    fpu_allocstate  = 0;
    fp_live_mask    = 0;
    fp_dirty_mask   = 0;
    fp_written_mask = 0;
    reg_FPCR = 0xff; mod_FPCR = 0;
    reg_FPSR = 0xff; mod_FPSR = 0;
}

/* ---- CTX: Emu68's RA_GetCTX does `mrs reg, TPIDRRO_EL0`. Ours returns x0 (the state
 * pointer is passed in x0, not a system register). Our driven opcodes never call this,
 * but provide it for completeness / future memory opcodes. ----------------------------- */
static uint8_t reg_CTX = 0xff;
uint8_t RA_TryCTX(uint32_t **ptr) { (void)ptr; return reg_CTX; }
uint8_t RA_GetCTX(uint32_t **ptr) { (void)ptr; reg_CTX = J5C_STATE_X /*x1*/; return reg_CTX; }
void    RA_FlushCTX(uint32_t **ptr) { (void)ptr; reg_CTX = 0xff; }

/* ===================================================================================
 * [J5o] THE FPU REGISTER ALLOCATOR — the 68881/68882 FP register file + FPCR/FPSR.
 *
 * This is now a REAL implementation (the prior [J5c] version was a 0xff no-op stub,
 * harmless because the integer corpus never drives the FPU decoder). The contract is the
 * same shape as the integer RA above, re-hosted for the FPU:
 *
 *   FP0..FP7 map to a FIXED set of AArch64 d-registers — d8..d15 — exactly as Emu68's
 *   RA_MapFPURegister does (`return (fpu_reg & 7) + 8`). d8..d15 are AAPCS64 CALLEE-SAVED,
 *   so the FP file stays live across the JIT'd block without per-op spill (mirrors how the
 *   integer file lives in callee-saved w19.. across the block). The engine's prologue seeds
 *   d8..d15 from st->fp[] and the epilogue stores them back — but ONLY for the FP registers
 *   the block actually touches (the [J5e]-style liveness masks below), so a block that uses
 *   no FPU emits ZERO FP load/store and the integer corpus is byte-for-byte unchanged.
 *
 *   FPCR / FPSR are MEMORY-BACKED in struct j5d_m68k_state (offsets J5O_OFF_FPCR/FPSR),
 *   loaded lazily into a scratch reg and stored back if modified — exactly like the CCR
 *   above (NOT Emu68's bare-metal v29-SIMD packing, the FPU analogue of the TPIDR coupling).
 *
 *   Scratch FP registers (RA_AllocFPURegister) come from d2..d7 — disjoint from the
 *   FP0..FP7 file (d8..d15) and from the AArch64 argument/temporary d0..d1 the runtime
 *   helper-call paths use. This matches Emu68's own scratch FP pool (`for i=2; i<8`).
 * =================================================================================== */

/* Byte offsets of fpcr/fpsr in struct j5d_m68k_state (the append-only [J5o] fields). The
 * engine STATIC-ASSERTS these against offsetof() at build time (j5d_engine.c), so this
 * literal can never silently drift from the struct. The state pointer is in x1 (J5C_STATE_X),
 * the same base the CCR load/store uses; the FPU fields sit past the integer file. */
#define J5O_OFF_FPCR  144u
#define J5O_OFF_FPSR  148u

/* (fpu_allocstate / fp_live_mask / fp_dirty_mask / fp_written_mask declared up top.) */

static inline void ra_note_fp_read(uint8_t f)
{
    uint16_t bit = (uint16_t)(1u << (f & 7));
    if (!(fp_written_mask & bit)) fp_live_mask |= bit;
}
static inline void ra_note_fp_write(uint8_t f)
{
    uint16_t bit = (uint16_t)(1u << (f & 7));
    fp_dirty_mask |= bit;
    fp_written_mask |= bit;
}

/* The engine reads these after driving the decoders for a block to size its FP frame. */
void j5c_ra_get_fp_masks(uint16_t *live, uint16_t *dirty)
{
    if (live)  *live  = fp_live_mask;
    if (dirty) *dirty = fp_dirty_mask;
}

void    RA_ResetFPUAllocator(void) { fpu_allocstate = 0; }
uint8_t RA_MapFPURegister(uint32_t **s, uint8_t f)          { (void)s; ra_note_fp_read(f);  return (uint8_t)((f & 7) + 8); }
uint8_t RA_MapFPURegisterForWrite(uint32_t **s, uint8_t f)  { (void)s; ra_note_fp_write(f); return (uint8_t)((f & 7) + 8); }
void    RA_SetDirtyFPURegister(uint32_t **s, uint8_t f)     { (void)s; ra_note_fp_write(f); }
void    RA_FlushFPURegs(uint32_t **s)                       { (void)s; }
void    RA_StoreDirtyFPURegs(uint32_t **s)                  { (void)s; }
uint8_t RA_AllocFPURegister(uint32_t **s)
{
    (void)s;
    for (int i = 2; i < 8; i++) if ((fpu_allocstate & (1 << i)) == 0) { fpu_allocstate |= 1 << i; return (uint8_t)i; }
    return 0xff;
}
void RA_FreeFPURegister(uint32_t **s, uint8_t a) { (void)s; if (a < 8 && (fpu_allocstate & (1 << a))) fpu_allocstate &= ~(1 << a); }

/* ---- FPCR / FPSR — MEMORY-BACKED (NOT Emu68's v29-SIMD packing) --------------------
 * Same lazy-load / store-if-modified scheme as the CCR (reg_CC above), reading/writing the
 * appended fpcr/fpsr fields of struct j5d_m68k_state via the x1 state base. The FPU decoder
 * only ever RA_ModifyFPSR()s when FPSR_Update_Needed says a consumer follows; for FPCR it
 * reads the rounding/precision mode (we store it; this increment leaves rounding at the
 * default-nearest the host also uses, so the result bit-exactness holds). (reg_FPCR/mod_FPCR/
 * reg_FPSR/mod_FPSR are declared up top — j5c_ra_reset resets them before these bodies.) */
uint8_t RA_GetFPCR(uint32_t **ptr)
{
    if (reg_FPCR == 0xff) {
        reg_FPCR = alloc_scratch();
        **ptr = ldr_offset(J5C_STATE_X, reg_FPCR, J5O_OFF_FPCR); (*ptr)++;
        mod_FPCR = 0;
    }
    return reg_FPCR;
}
uint8_t RA_ModifyFPCR(uint32_t **ptr) { uint8_t r = RA_GetFPCR(ptr); mod_FPCR = 1; return r; }
void    RA_StoreFPCR(uint32_t **ptr)
{
    if (reg_FPCR != 0xff && mod_FPCR) { **ptr = str_offset(J5C_STATE_X, reg_FPCR, J5O_OFF_FPCR); (*ptr)++; }
}
void    RA_FlushFPCR(uint32_t **ptr)
{
    if (reg_FPCR != 0xff) {
        if (mod_FPCR) { **ptr = str_offset(J5C_STATE_X, reg_FPCR, J5O_OFF_FPCR); (*ptr)++; }
        RA_FreeARMRegister(ptr, reg_FPCR);
    }
    reg_FPCR = 0xff; mod_FPCR = 0;
}

uint8_t RA_GetFPSR(uint32_t **ptr)
{
    if (reg_FPSR == 0xff) {
        reg_FPSR = alloc_scratch();
        **ptr = ldr_offset(J5C_STATE_X, reg_FPSR, J5O_OFF_FPSR); (*ptr)++;
        mod_FPSR = 0;
    }
    return reg_FPSR;
}
uint8_t RA_ModifyFPSR(uint32_t **ptr) { uint8_t r = RA_GetFPSR(ptr); mod_FPSR = 1; return r; }
void    RA_StoreFPSR(uint32_t **ptr)
{
    if (reg_FPSR != 0xff && mod_FPSR) { **ptr = str_offset(J5C_STATE_X, reg_FPSR, J5O_OFF_FPSR); (*ptr)++; }
}
void    RA_FlushFPSR(uint32_t **ptr)
{
    if (reg_FPSR != 0xff) {
        if (mod_FPSR) { **ptr = str_offset(J5C_STATE_X, reg_FPSR, J5O_OFF_FPSR); (*ptr)++; }
        RA_FreeARMRegister(ptr, reg_FPSR);
    }
    reg_FPSR = 0xff; mod_FPSR = 0;
}
