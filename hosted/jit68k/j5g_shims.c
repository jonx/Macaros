/* j5g_shims.c — [J5g] extra link stubs for the NEWLY vendored Emu68 decoders
 * (M68k_LINE0.c, M68k_LINE4.c, M68k_LINEE.c), so they compile + link hosted (GLUE,
 * OURS, AROS-licensed). The [J5c]/[J5d] shims (j5c_shims.c) already provide cache_read_*,
 * the PC helpers, M68K_GetSRMask, SR_GetEALength, EMIT_Exception, kprintf, __m68k_state.
 * [J5g] adds only the handful of EXTRA externals the new LINE files reference for
 * SUB-OPCODES THE ENGINE NEVER DRIVES (the dispatcher owns all control flow + tracing
 * is off) — they exist purely so the verbatim files LINK.
 *
 * LICENCE BOUNDARY (see emu68/NOTICE, docs/features/CLEANROOM.md): THIS FILE is OURS. It
 * defines symbols the vendored decoders reference; no Emu68 function body is copied. Each
 * stub is the documented no-op / off contract. (MPL FAQ Q11-Q13: linking does not
 * relicense this file.)
 *
 * WHY EACH STUB IS NEVER ON A HOT PATH:
 *   debug / debug_range_min / debug_range_max / disasm
 *       — referenced ONLY by EMIT_MOVEC in M68k_LINE4.c (a privileged control-register
 *         move), which the [J5g] engine never drives (no privileged opcodes in the
 *         corpus/demanding program). They are a debug-trace gate; `debug = 0` keeps every
 *         tracing branch off. `disasm` is referenced by ADDRESS only (never called).
 *   M68K_PopReturnAddress
 *       — referenced ONLY by EMIT_RTS in M68k_LINE4.c. The engine DECODES rts at the
 *         dispatcher level (the real 68k return stack, j5d_engine.c) and never drives
 *         EMIT_RTS, so this body is never reached. Returns NULL (no Emu68 return-address
 *         stack exists in the hosted model).
 */
#include <stdint.h>

/* EMIT_MOVEC tracing gate (off) + the disasm symbol it takes the address of. */
int      debug = 0;
uint32_t debug_range_min = 0;
uint32_t debug_range_max = 0;
void     disasm(void) { }

/* EMIT_RTS's software-return-stack pop — never driven (dispatcher owns rts). */
uint16_t *M68K_PopReturnAddress(uint8_t *success)
{
    if (success) *success = 0;
    return (uint16_t *)0;
}
