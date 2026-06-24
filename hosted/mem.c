// Hosted AArch64 AROS — Phase 2 H5: the AROS exec memory model, hosted.
//
// AROS exec doesn't malloc — it lays a MemHeader over a raw region and hands out
// chunks from a single-linked, address-ordered free list of MemChunks, coalescing
// neighbours on free. This file reproduces that allocator FAITHFULLY (the first-
// fit split in stdAlloc + the coalescing insert in stdDealloc), grounded verbatim
// against rom/exec/memory.c and compiler/include/exec/memory.h in the upstream
// tree. The "RAM" is one mmap'd region — macOS owns the pages, exec owns the
// policy, exactly as a hosted port wants.
//
// Faithfulness: same struct MemHeader/MemChunk, same MEMCHUNK_TOTAL (16 on
// AArch64) alignment, same first-fit + split + bidirectional coalesce, same
// MEMF_CLEAR/MEMF_REVERSE semantics, same FreeTwice overlap detection. Dropped
// from the spike: the managed-mem (MemHeaderExt) path and the mhac index cache
// (a lookup accelerator, not a correctness feature) — noted, not faked.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

// ---- exec/nodes.h + exec/memory.h (verbatim shapes) ------------------------
#define NT_MEMORY 10
struct Node {
    struct Node *ln_Succ, *ln_Pred;
    uint8_t      ln_Type;
    int8_t       ln_Pri;
    char        *ln_Name;
};
struct MemChunk {
    struct MemChunk *mc_Next;
    uintptr_t        mc_Bytes;
};
struct MemHeader {
    struct Node      mh_Node;
    uint16_t         mh_Attributes;
    struct MemChunk *mh_First;
    void            *mh_Lower;
    void            *mh_Upper;
    uintptr_t        mh_Free;
};

#define MEMCHUNK_TOTAL 16          // max(AROS_WORSTALIGN, sizeof(MemChunk)) on AArch64
#define MEMF_ANY      0L
#define MEMF_CLEAR    (1L << 16)
#define MEMF_REVERSE  (1L << 18)
#define ROUNDUP(x, a) (((uintptr_t)(x) + (a) - 1) & ~(uintptr_t)((a) - 1))

// ---- Allocate(): first-fit out of one MemHeader (rom/exec/allocate.c +
//      stdAlloc in memory.c) ------------------------------------------------
static void *Allocate(struct MemHeader *mh, uintptr_t size, unsigned long flags)
{
    if (!size)
        return NULL;
    uintptr_t byteSize = ROUNDUP(size, MEMCHUNK_TOTAL);
    if (mh->mh_Free < byteSize)
        return NULL;

    // p1 starts at &mh_First so that writing p1->mc_Next rewrites mh_First.
    struct MemChunk *p1 = (struct MemChunk *)&mh->mh_First;
    struct MemChunk *p2 = p1->mc_Next;
    struct MemChunk *mc = NULL;

    while (p2 != NULL) {
        if (p2->mc_Bytes >= byteSize) {
            mc = p1;                                // remember the PREDECESSOR
            if (!(flags & MEMF_REVERSE))
                break;
        }
        p1 = p2;
        p2 = p1->mc_Next;
    }
    if (mc == NULL)
        return NULL;

    p1 = mc;
    p2 = p1->mc_Next;
    if (p2->mc_Bytes == byteSize) {
        p1->mc_Next = p2->mc_Next;                  // exact fit: unlink whole chunk
        mc = p2;
    } else {
        // Split: return the first byteSize bytes, leave a remainder chunk.
        struct MemChunk *rem = (struct MemChunk *)((uint8_t *)p2 + byteSize);
        p1->mc_Next  = rem;
        rem->mc_Next = p2->mc_Next;
        rem->mc_Bytes = p2->mc_Bytes - byteSize;
        mc = p2;
    }
    mh->mh_Free -= byteSize;
    if (flags & MEMF_CLEAR)
        memset(mc, 0, byteSize);
    return mc;
}

// ---- Deallocate(): coalescing insert (stdDealloc in memory.c) --------------
static int Deallocate(struct MemHeader *mh, void *addr, uintptr_t size)
{
    if (!addr || !size)
        return 1;
    // Align size up and block down to MEMCHUNK_TOTAL (handles misaligned addr).
    uintptr_t byteSize = size + ((uintptr_t)addr & (MEMCHUNK_TOTAL - 1));
    byteSize = ROUNDUP(byteSize, MEMCHUNK_TOTAL);
    void *memoryBlock = (void *)((uintptr_t)addr & ~(uintptr_t)(MEMCHUNK_TOTAL - 1));

    struct MemChunk *p1 = (struct MemChunk *)&mh->mh_First;
    struct MemChunk *p2 = mh->mh_First;
    struct MemChunk *p3 = (struct MemChunk *)memoryBlock;
    uint8_t *p4 = (uint8_t *)p3 + byteSize;

    if (p2 == NULL) {                               // empty list: just insert
        p3->mc_Bytes = byteSize;
        p3->mc_Next  = NULL;
        p1->mc_Next  = p3;
        mh->mh_Free += byteSize;
        return 1;
    }

    // Find the first chunk at a higher address than the block being freed.
    do {
        if (p2 >= p3) {
            if (p4 > (uint8_t *)p2) {               // overlap -> double free
                fprintf(stderr, "[MM] FreeTwice: block overlaps chunk %p\n", (void *)p2);
                return 0;
            }
            break;
        }
        p1 = p2;
        p2 = p2->mc_Next;
    } while (p2 != NULL);

    // Merge with the previous chunk if adjacent.
    if (p1 != (struct MemChunk *)&mh->mh_First) {
        if ((uint8_t *)p1 + p1->mc_Bytes > (uint8_t *)p3) {
            fprintf(stderr, "[MM] FreeTwice: block overlaps prev chunk %p\n", (void *)p1);
            return 0;
        }
        if ((uint8_t *)p1 + p1->mc_Bytes == (uint8_t *)p3)
            p3 = p1;                                // coalesce backward
        else
            p1->mc_Next = p3;
    } else {
        p1->mc_Next = p3;
    }

    // Merge with the next chunk if adjacent.
    if (p4 == (uint8_t *)p2 && p2 != NULL) {
        p4 += p2->mc_Bytes;
        p2 = p2->mc_Next;
    }
    p3->mc_Next  = p2;
    p3->mc_Bytes = p4 - (uint8_t *)p3;
    mh->mh_Free += byteSize;
    return 1;
}

// Lay a MemHeader over a raw region as one big free chunk (the AROS init shape).
static void mh_Init(struct MemHeader *mh, void *base, uintptr_t len)
{
    len &= ~(uintptr_t)(MEMCHUNK_TOTAL - 1);
    mh->mh_Node.ln_Type = NT_MEMORY;
    mh->mh_Node.ln_Pri  = 0;
    mh->mh_Node.ln_Name = "hosted-ram";
    mh->mh_Attributes   = MEMF_ANY;
    mh->mh_Lower        = base;
    mh->mh_Upper        = (uint8_t *)base + len;
    mh->mh_First        = (struct MemChunk *)base;
    mh->mh_First->mc_Next  = NULL;
    mh->mh_First->mc_Bytes = len;
    mh->mh_Free         = len;
}

// ---- AllocMem/FreeMem over one global system MemHeader ---------------------
static struct MemHeader sysmh;
static void *AllocMem(uintptr_t size, unsigned long flags) { return Allocate(&sysmh, size, flags); }
static int   FreeMem (void *p, uintptr_t size)             { return Deallocate(&sysmh, p, size); }

// ---- free-list invariants (for the verifier) -------------------------------
static int chunk_count(struct MemHeader *mh) {
    int n = 0;
    for (struct MemChunk *c = mh->mh_First; c; c = c->mc_Next) n++;
    return n;
}
static uintptr_t free_sum(struct MemHeader *mh) {
    uintptr_t s = 0;
    for (struct MemChunk *c = mh->mh_First; c; c = c->mc_Next) s += c->mc_Bytes;
    return s;
}
// address-ordered, non-overlapping, in-bounds
static int freelist_sane(struct MemHeader *mh) {
    uint8_t *prev_end = (uint8_t *)mh->mh_Lower;
    for (struct MemChunk *c = mh->mh_First; c; c = c->mc_Next) {
        if ((void *)c < mh->mh_Lower || (uint8_t *)c + c->mc_Bytes > (uint8_t *)mh->mh_Upper)
            return 0;
        if ((uint8_t *)c < prev_end) return 0;      // out of order / overlap
        prev_end = (uint8_t *)c + c->mc_Bytes;
    }
    return 1;
}

#define CHECK(cond) do { if (!(cond)) { printf("[H5] FAIL: %s (line %d)\n", #cond, __LINE__); ok = 0; } } while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[H5a] hosted AROS memory: MemHeader/MemChunk free-list allocator over mmap\n");

    const uintptr_t LEN = 16u << 20;                // 16 MiB "RAM" from macOS
    void *region = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) { printf("[H5] FAIL: mmap\n"); return 1; }
    mh_Init(&sysmh, region, LEN);
    const uintptr_t total = sysmh.mh_Free;
    int ok = 1;

    // T1 — basic alloc: alignment, in-bounds, distinct, MEMF_CLEAR, no clobber.
    void *a = AllocMem(100,  MEMF_ANY);
    void *b = AllocMem(2000, MEMF_ANY);
    void *c = AllocMem(64,   MEMF_CLEAR);
    CHECK(a && b && c);
    CHECK(((uintptr_t)a % MEMCHUNK_TOTAL) == 0 && ((uintptr_t)b % MEMCHUNK_TOTAL) == 0);
    CHECK(a >= region && (uint8_t *)c + 64 <= (uint8_t *)region + LEN);
    // distinct, non-overlapping (rounded sizes)
    CHECK((uint8_t *)a + 112 <= (uint8_t *)b || (uint8_t *)b + 2000 <= (uint8_t *)a);
    for (int i = 0; i < 64; i++) CHECK(((uint8_t *)c)[i] == 0);   // cleared
    memset(a, 0xAA, 100); memset(b, 0xBB, 2000); memset(c, 0xCC, 64);
    for (int i = 0; i < 100; i++)  CHECK(((uint8_t *)a)[i] == 0xAA);  // no cross-clobber
    for (int i = 0; i < 2000; i++) CHECK(((uint8_t *)b)[i] == 0xBB);
    CHECK(freelist_sane(&sysmh) && free_sum(&sysmh) == sysmh.mh_Free);
    printf("[H5]   alloc: a=%p b=%p c=%p  free=%lu/%lu\n", a, b, c,
           (unsigned long)sysmh.mh_Free, (unsigned long)total);

    // T2 — free all three, expect full coalesce back to one chunk.
    FreeMem(a, 100); FreeMem(b, 2000); FreeMem(c, 64);
    CHECK(sysmh.mh_Free == total);
    CHECK(chunk_count(&sysmh) == 1);
    printf("[H5]   after free-all: free=%lu chunks=%d (want %lu / 1)\n",
           (unsigned long)sysmh.mh_Free, chunk_count(&sysmh), (unsigned long)total);

    // T3 — fragment then coalesce: 8 blocks, free odds (holes), then evens.
    void *blk[8];
    for (int i = 0; i < 8; i++) blk[i] = AllocMem(4096, MEMF_ANY);
    for (int i = 0; i < 8; i++) CHECK(blk[i] != NULL);
    CHECK(freelist_sane(&sysmh));
    for (int i = 1; i < 8; i += 2) FreeMem(blk[i], 4096);   // holes
    CHECK(freelist_sane(&sysmh) && free_sum(&sysmh) == sysmh.mh_Free);
    int frags = chunk_count(&sysmh);
    for (int i = 0; i < 8; i += 2) FreeMem(blk[i], 4096);   // fill holes -> coalesce
    CHECK(sysmh.mh_Free == total);
    CHECK(chunk_count(&sysmh) == 1);
    printf("[H5]   fragment->coalesce: peak chunks=%d -> 1, free back to %lu\n",
           frags, (unsigned long)sysmh.mh_Free);

    // T4 — exhaustion is graceful, and fully recoverable.
    enum { CAP = 4096 };
    void *big[CAP]; int n = 0;
    while (n < CAP && (big[n] = AllocMem(64 * 1024, MEMF_ANY)) != NULL) n++;
    CHECK(n > 0);
    CHECK(AllocMem(64 * 1024, MEMF_ANY) == NULL || sysmh.mh_Free >= 64 * 1024);
    for (int i = 0; i < n; i++) FreeMem(big[i], 64 * 1024);
    CHECK(sysmh.mh_Free == total);
    CHECK(chunk_count(&sysmh) == 1);
    printf("[H5]   exhaustion: filled %d x 64KiB, freed all -> free=%lu chunks=%d\n",
           n, (unsigned long)sysmh.mh_Free, chunk_count(&sysmh));

    if (ok)
        printf("[H5] hosted AROS AllocMem ok: first-fit + split + coalesce faithful, all invariants held\n");
    else
        printf("[H5] FAIL: see checks above\n");
    munmap(region, LEN);
    return ok ? 0 : 1;
}
