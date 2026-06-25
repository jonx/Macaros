/* j5c_shims.c — [J5c] hosted replacements for Emu68's bare-metal runtime hooks, so the
 * REAL Emu68 decoders (emu68/M68k_LINE*.c, M68k_MOVE.c, M68k_EA.c) compile + run hosted
 * (GLUE, OURS, AROS-licensed).
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS.
 * It #includes the quarantined Emu68 headers (for the struct/enum/constant *types* the
 * ABI needs — types are facts, not copyrightable expression) and DEFINES the external
 * symbols the vendored decoders reference. No Emu68 function BODY is copied here; each
 * shim is authored from the documented contract (the [J5a] finding + the m68k/AArch64
 * ISA). Calling/linking the vendored decoders does not relicense this file (MPL FAQ
 * Q11-Q13).
 *
 * This file provides:
 *   HOOK 2  cache_read_16 / _8 / _32  — direct BIG-ENDIAN reads from the host pointer the
 *           decoder passes (Emu68 passes &m68k_ptr[i] cast to uintptr_t — a HOST pointer
 *           into the instruction stream, NOT a 68k address). No software ICache.
 *   the PC helpers EMIT_AdvancePC / EMIT_FlushPC / EMIT_GetOffsetPC / EMIT_ResetOffsetPC
 *           — minimal REG_PC bookkeeping (our register block never reads PC, so these
 *           just keep _pc_rel and emit the same add/sub REG_PC the real translator would).
 *   M68K_GetSRMask  — returns SR_CCR (all five CCR bits "set by this opcode"), the
 *           CONSERVATIVE answer that makes every decoder take its full-flag-compute path.
 *           This deliberately severs the M68k_SR.c coupling (which would drag in all 16
 *           GetSR_LineX + M68K_GetLineXLength + M68K_IsBranch). Correct, just not optimal.
 *   SR_GetEALength  — extension-word count for an EA (our block is register-direct => 0;
 *           the few non-reg modes we never reach are handled conservatively).
 *   EMIT_Exception / EMIT_InjectDebugString  — no-op-ish stubs (only reached on the
 *           ILLEGAL-INSTRUCTION error path, never for the valid opcodes we drive).
 *   __m68k_state  — the global context pointer the decoders read for JIT_CONTROL tunables
 *           (we point it at a zeroed struct so all the "slowdown"/debug paths stay off).
 *   kprintf  — no-op (A64.h ASSERT_REG / decoder error paths; never called for valid regs).
 */
#include <stdint.h>
#include <stdarg.h>

#include "emu68/M68k.h"          /* struct M68KState, VECTOR_*, SR_* (types/constants) */
#include "emu68/cache.h"         /* enum CacheType, cache_read_* signatures            */
#include "emu68/A64.h"           /* REG_PC, encoders (add_immed/sub_immed)             */

/* ---- the global context the decoders read (JIT_CONTROL tunables, all zero = off) ---- */
struct M68KState  g_j5c_dummy_state;
struct M68KState *__m68k_state = &g_j5c_dummy_state;

/* insn_count: the translator's running m68k-instruction counter (M68k_MULDIV.c reads it
 * for its cycle bookkeeping). We don't model cycles; a dummy backing storage suffices. */
uint32_t insn_count = 0;

/* ---- kprintf: the single debug symbol A64.h / decoder error paths reference --------- */
void kprintf(const char *format, ...) { (void)format; }

/* ===================================================================================
 * HOOK 2 — instruction fetch.  Big-endian read from the host instruction stream.
 *
 * A DEEPER FETCH COUPLING than [J5a] named: Emu68's cache_read_16 takes a **uint32_t**
 * `address` — bare-metal the 68k space is 32-bit and maps 1:1 to low physical RAM, so a
 * 32-bit address IS the host pointer. Hosted, the instruction stream lives at a 64-BIT
 * host address; the decoders pass `(uintptr_t)&m68k_ptr[i]` which the uint32_t parameter
 * TRUNCATES, destroying the pointer. We cannot change the vendored signature, so we
 * REGISTER the high 32 bits of the stream's host base (j5c_fetch_set_base) and splice
 * them back here: full_ptr = (base_hi << 32) | (uint32_t)address. This works because the
 * whole stream lives within one 4 GiB-aligned host window (a single malloc/region), so
 * the high half is constant. The 68k stream is stored big-endian, so a BE assembly of
 * the bytes yields the architectural opcode on our little-endian host. NO Emu68 software
 * ICache (cache.c) is linked; this is the whole "cache".
 * =================================================================================== */
static uint64_t g_fetch_base_hi = 0;   /* high 32 bits of the instruction-stream host ptr */

void j5c_fetch_set_base(const void *host_stream)
{
    g_fetch_base_hi = ((uint64_t)(uintptr_t)host_stream) & 0xFFFFFFFF00000000ull;
}

static inline const uint8_t *fetch_ptr(uint32_t address)
{
    return (const uint8_t *)(uintptr_t)(g_fetch_base_hi | (uint64_t)address);
}

uint16_t cache_read_16(enum CacheType type, uint32_t address)
{
    (void)type;
    const uint8_t *p = fetch_ptr(address);
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint8_t cache_read_8(enum CacheType type, uint32_t address)
{
    (void)type;
    return *fetch_ptr(address);
}

uint32_t cache_read_32(enum CacheType type, uint32_t address)
{
    (void)type;
    const uint8_t *p = fetch_ptr(address);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* ===================================================================================
 * PC bookkeeping shims (live in M68k_Translator.c upstream — re-hosted here, minimal).
 * Our register-direct block never reads the 68k PC, but the decoders still call
 * EMIT_AdvancePC after each opcode; we keep the same _pc_rel accumulator + REG_PC
 * add/sub the real translator emits, so the emitted block is well-formed.
 * =================================================================================== */
int32_t _pc_rel = 0;

uint32_t *EMIT_AdvancePC(uint32_t *ptr, uint8_t offset)
{
    _pc_rel += (int)offset;
    if (_pc_rel > 120 || _pc_rel < -120) {
        if (_pc_rel > 0) *ptr++ = add_immed(REG_PC, REG_PC, _pc_rel);
        else             *ptr++ = sub_immed(REG_PC, REG_PC, -_pc_rel);
        _pc_rel = 0;
    }
    return ptr;
}

uint32_t *EMIT_FlushPC(uint32_t *ptr)
{
    if (_pc_rel > 0)      *ptr++ = add_immed(REG_PC, REG_PC, _pc_rel);
    else if (_pc_rel < 0) *ptr++ = sub_immed(REG_PC, REG_PC, -_pc_rel);
    _pc_rel = 0;
    return ptr;
}

uint32_t *EMIT_GetOffsetPC(uint32_t *ptr, int8_t *offset)
{
    int new_offset = _pc_rel + *offset;
    if (new_offset > 127 || new_offset < -127) {
        if (_pc_rel > 0) *ptr++ = add_immed(REG_PC, REG_PC, _pc_rel);
        else             *ptr++ = sub_immed(REG_PC, REG_PC, -_pc_rel);
        _pc_rel = 0;
        new_offset = *offset;
    }
    *offset = new_offset;
    return ptr;
}

uint32_t *EMIT_ResetOffsetPC(uint32_t *ptr) { _pc_rel = 0; return ptr; }

/* ===================================================================================
 * M68K_GetSRMask — conservative "this opcode sets all CCR bits". Forces every decoder
 * onto its full-flag path and severs the M68k_SR.c all-16-lines coupling.
 * =================================================================================== */
uint8_t M68K_GetSRMask(uint16_t *m68k_stream) { (void)m68k_stream; return SR_CCR; }

/* SR_GetEALength — extension words for an EA. Our richer block is register-direct
 * (modes 0/1 => 0 ext words). Non-reg modes are not reached; return 0 conservatively
 * (the driver validates the block is register-direct before running). */
uint8_t SR_GetEALength(uint16_t *insn_stream, uint8_t ea, uint8_t imm_size)
{
    (void)insn_stream; (void)ea; (void)imm_size;
    return 0;
}

/* M68K_GetINSNLength — full instruction length in words. Lives in the un-vendored
 * M68k_SR.c (which fans out to all 16 lines' length functions — the coupling we sever
 * via M68K_GetSRMask). EMIT_move references it for some EA-displacement paths that our
 * register-direct (moveq) block never reaches; 1 word is the correct length for an
 * extension-word-free register-direct opcode. (Reached on no hot path for [J5c].) */
int M68K_GetINSNLength(uint16_t *insn_stream) { (void)insn_stream; return 1; }

/* ===================================================================================
 * EMIT_Exception / EMIT_InjectDebugString — only on the ILLEGAL-INSTRUCTION error path
 * (never for the valid opcodes [J5c] drives). Emit nothing; a block-exit sentinel is
 * appended by the caller's error handling, not here.
 * =================================================================================== */
uint32_t *EMIT_Exception(uint32_t *ptr, uint16_t exception, uint8_t format, ...)
{
    (void)exception; (void)format;
    return ptr;
}

uint32_t *EMIT_InjectDebugString(uint32_t *ptr, const char *restrict format, ...)
{
    (void)format;
    return ptr;
}
