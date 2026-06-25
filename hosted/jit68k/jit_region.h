/* jit_region.h — W^X-aware executable-memory layer for the hosted 68k JIT.
 *
 * Clean-room [OURS]. Authored from docs/features/68k-jit/spec.md (the [J1]
 * MAP_JIT executable-memory layer + its W^X / threading section) and Apple/POSIX
 * docs only. This file contains NO Emu68 / UAE / vAmiga source — it is the
 * native-macOS executable-memory substrate the adapted Emu68 emitter ([J2]) and
 * the native LoadSeg path will both sit on top of.
 *
 * Contract (the spec's R-JIT-* requirements):
 *   R-JIT-MAP    a MAP_JIT region is the only W^X-legal source of executable JIT
 *                memory under the hardened runtime.
 *   R-JIT-WRITE  writes happen ONLY inside a pthread_jit_write_protect_np(0/1)
 *                window; the toggle is PER-THREAD (R-JIT-THREAD: emit and execute
 *                must be on the same host thread — satisfied by the H6 single
 *                scheduler thread).
 *   R-JIT-ICACHE after patching and before executing a range, sys_icache_invalidate
 *                it — arm64 I/D caches are NOT coherent for self-written code, so
 *                skipping this yields a stale-I-cache WRONG VALUE, not a crash.
 *
 * Usage shape (what [J2]'s emitter and LoadSeg will call):
 *   jit_region r;
 *   jit_region_alloc(&r, size);
 *   jit_write_begin(&r);                 // open the per-thread writable window
 *   ... write AArch64 words into r.base ...
 *   jit_write_end(&r);                   // close the window (region executable)
 *   jit_finalize(&r, ptr, len);          // i-cache invalidate the patched range
 *   ((fn)ptr)();                         // now safe to execute
 *   jit_region_free(&r);
 */
#ifndef JIT_REGION_H
#define JIT_REGION_H

#include <stddef.h>
#include <stdint.h>

typedef struct jit_region {
    void  *base;   /* start of the MAP_JIT mapping (PROT_READ|EXEC, writable via toggle) */
    size_t size;   /* page-rounded length of the mapping */
} jit_region;

/* R-JIT-MAP: mmap a MAP_JIT region of at least `size` bytes (page-rounded).
 * Returns 0 on success, -1 on failure (errno set). The region is mapped
 * PROT_READ|PROT_WRITE|PROT_EXEC with MAP_JIT — under the hardened runtime it is
 * effectively R-X until a write window is opened (see jit_write_begin). */
int  jit_region_alloc(jit_region *r, size_t size);

/* Release a region from jit_region_alloc. Safe on a zeroed/failed region. */
void jit_region_free(jit_region *r);

/* R-JIT-WRITE: open the per-thread writable window over ALL MAP_JIT pages on the
 * calling thread (pthread_jit_write_protect_np(0)). Must be paired with
 * jit_write_end on the SAME thread (R-JIT-THREAD). */
void jit_write_begin(jit_region *r);

/* R-JIT-WRITE: close the writable window (pthread_jit_write_protect_np(1)); the
 * MAP_JIT pages become executable again on this thread. */
void jit_write_end(jit_region *r);

/* R-JIT-ICACHE: make a freshly-written [ptr, ptr+len) range safe to execute by
 * invalidating the instruction cache over it. Call AFTER jit_write_end and
 * BEFORE casting `ptr` to a function pointer and calling it. */
void jit_finalize(jit_region *r, void *ptr, size_t len);

/* Convenience encoders for the [J1] hand-assembled stub (and a sanity anchor for
 * [J2]'s adopted emitter). These compute the exact AArch64 instruction word from
 * the architectural encoding — they do NOT hardcode a magic constant.
 *
 * JIT_MOVZ_W0(imm16): `movz w0, #imm16`  -> 32-bit MOVZ, sf=0 (Wd), hw=0, Rd=0.
 *   Encoding: 0x52800000 | (imm16 << 5) | Rd(=0).  (Verified against the Apple
 *   assembler: movz w0,#0x6804 == 0x528D0080.)
 * JIT_RET: `ret` (return to x30). Fixed word 0xD65F03C0. */
static inline uint32_t JIT_MOVZ_W0(uint16_t imm16) {
    return 0x52800000u | ((uint32_t)imm16 << 5);
}
#define JIT_RET 0xD65F03C0u

#endif /* JIT_REGION_H */
