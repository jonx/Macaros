/* jit_region.c — W^X-aware executable-memory layer (implementation).
 *
 * Clean-room [OURS]. See jit_region.h for the contract. Pure Apple/POSIX: mmap +
 * MAP_JIT, pthread_jit_write_protect_np, sys_icache_invalidate. No Emu68/UAE/vAmiga.
 */
#include "jit_region.h"

#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include <stdint.h>

/* The minimal W^X dance, in one place so [J2]'s emitter and LoadSeg reuse it. */

static size_t page_round_up(size_t n)
{
    long pg = sysconf(_SC_PAGESIZE);
    size_t p = (pg > 0) ? (size_t)pg : 16384u;   /* Apple-silicon native page = 16 KiB */
    return (n + p - 1) & ~(p - 1);
}

int jit_region_alloc(jit_region *r, size_t size)
{
    r->base = MAP_FAILED;
    r->size = 0;

    size_t len = page_round_up(size ? size : 1);

    /* R-JIT-MAP. MAP_JIT is the only W^X-legal route to executable JIT memory
     * under the hardened runtime. Request RWX: with the allow-jit entitlement +
     * MAP_JIT the kernel honours this (the per-thread toggle then gates *write*
     * access); without it, this mmap or a later write/exec faults — which is
     * exactly the R-JIT-ENTITLE question [J1] pins. */
    void *p = mmap(NULL, len,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT,
                   -1, 0);
    if (p == MAP_FAILED)
        return -1;

    r->base = p;
    r->size = len;
    return 0;
}

void jit_region_free(jit_region *r)
{
    if (r && r->base && r->base != MAP_FAILED && r->size) {
        munmap(r->base, r->size);
        r->base = MAP_FAILED;
        r->size = 0;
    }
}

void jit_write_begin(jit_region *r)
{
    (void)r;                              /* the toggle is per-thread, not per-region */
    pthread_jit_write_protect_np(0);      /* R-JIT-WRITE: open the writable window */
}

void jit_write_end(jit_region *r)
{
    (void)r;
    pthread_jit_write_protect_np(1);      /* R-JIT-WRITE: close it (pages executable) */
}

void jit_finalize(jit_region *r, void *ptr, size_t len)
{
    (void)r;
    /* R-JIT-ICACHE. I/D caches are not coherent for self-written code on arm64;
     * without this the CPU may fetch stale instructions (wrong VALUE, not a
     * crash) — which is why [J1] asserts the returned constant, not "no crash". */
    sys_icache_invalidate(ptr, len);
}
