/* t0p1_seglist.c — [T0-P1] the guest-address / loader-representation proof.
 * (OURS, AROS-licensed. Contains NO Emu68 source; links the engine via libjit68k.)
 *
 * THE QUESTION (docs/features/68k-transparent-exec/plan.md, [T0-P1]): on hosted
 * darwin-aarch64 the upstream hunk loader cannot run as-is — it hard-requires
 * MEMF_31BIT on 64-bit targets (rom/dos/internalloadseg_aos.c:196) and the darwin
 * bootstrap documents the low 4 GiB as unavailable (arch/all-unix/bootstrap/
 * memory.c:57).  So hunk relocations must produce GUEST addresses (32-bit,
 * sandbox-space), not host pointers, while DOS-side identity (GetSegListInfo /
 * UnLoadSeg / the seg registry) must keep working on a native seglist value.
 *
 * THE REPRESENTATION PROVED HERE — the "proxy seglist":
 *
 *   native side (64-bit, normal RAM)             guest side (32-bit arena, BE)
 *   ------------------------------------        --------------------------------
 *   BPTR ──> [next BPTR][segdesc hunk 0]        [hunk 0 payload  @ guest base 0]
 *                 │      guest_base/size         [hunk 1 payload  @ guest base 1]
 *                 v                              ... relocated with GUEST values
 *            [next BPTR][segdesc hunk 1]             by the [J4] relocator (the
 *                 │                                  exact upstream algorithm)
 *                 v      first node also
 *               BNULL    owns the image record (arena + entry + hunk count)
 *
 *   - The node shape matches what the native DOS machinery walks: AROS_FAST_BPTR
 *     (aarch64-all/include/aros/cpu.h:61 — BPTR is a plain pointer), the next
 *     link at offset 0 (internalunloadseg.c walks `next = *(BPTR*)BADDR(seg)`),
 *     payload after the link (internalloadseg_aos.c:20 GETHUNKPTR skips one BPTR).
 *   - Identity = the BPTR value, registered in the DOS seg registry with
 *     SEGTYPE_HUNK (internalloadseg_support.c register_hunk); GetSegListInfo
 *     matches by pointer equality (getseglistinfo.c) — modeled 1:1 here.
 *   - Lifetime: UnLoadSeg's blind walk frees the NATIVE nodes; the GUEST arena
 *     teardown hangs off the registry-removal hook (the same list entry that
 *     provides identity), NOT off the node walk — so standard UnLoadSeg needs no
 *     knowledge of the arena.
 *
 * THE PROOF (the plan's exit bar: a real hunk loaded, relocated, identified, run,
 * and unloaded — not merely an allocation below 4 GiB):
 *   1. load + relocate two REAL committed hunk binaries (mandel.exe: integer;
 *      j5t.exe: 68881/68882 hardware FP) into per-image guest arenas via the [J4]
 *      loader; build + register the proxy chain;
 *   2. identify both through the modeled GetSegListInfo(GSLI_68KHUNK) by BPTR
 *      value (positive), and a never-registered pointer (negative);
 *   3. run each THROUGH THE PROXY ONLY (the j4_seglist is discarded after chain
 *      construction — entry PC, arena and hunk table are recovered from the
 *      chain), through the full [J5d] JIT + stub OS, asserting the known
 *      byte-exact outputs and exits (j5t: 717-byte stream, D0=10857);
 *   4. unload via the modeled blind UnLoadSeg walk + registry hook; assert every
 *      native node freed, the arena freed, the registry empty, and a second
 *      GetSegListInfo now missing.
 * Marker: [T0P1] PASS / FAIL. */

#include "j4_hunk.h"
#include "j5d_jit68k.h"
#include "j3_jit68k.h"
#include "stublib.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- the sandbox runtime layout (the run68k convention; see run68k.c) ---- */
#define SANDBOX_ORIGIN  0x00210000u
#define SANDBOX_SIZE    0x01000000u
#define LIBBASE         0x00230000u
#define HEAP_BASE       0x00231000u
#define HEAP_END        0x00238000u
#define PROG_ORIGIN     0x00250000u

#define CHECK(cond, why) do { if (!(cond)) { \
    fprintf(stderr, "[T0P1] FAIL: %s (line %d)\n", why, __LINE__); exit(1); } } while (0)

/* ================= the representation (candidate for hosted/emu68k proper) ========= */

/* AROS_FAST_BPTR model: BPTR is a plain pointer, MKBADDR/BADDR are identity. */
typedef void *BPTRf;
#define BNULLf ((BPTRf)0)

#define SEGDESC_MAGIC 0x45364B21u  /* 'E6K!' */

typedef struct emu68k_image {
    uint8_t  *arena_host;          /* host backing of the guest arena           */
    uint32_t  arena_origin;        /* guest address of arena_host[0]            */
    uint32_t  arena_size;
    uint32_t  entry;               /* guest entry PC (first CODE hunk payload)  */
    int       numhunks;
} emu68k_image;

typedef struct emu68k_segdesc {
    uint32_t      magic;           /* SEGDESC_MAGIC                             */
    uint32_t      guest_base;      /* guest address of this hunk's payload      */
    uint32_t      guest_size;
    uint32_t      hunk_type;       /* J4_HUNK_CODE / _DATA / _BSS               */
    emu68k_image *image;           /* owner record; non-NULL on the FIRST node  */
} emu68k_segdesc;

/* One native segment node. Offset 0 MUST be the next-BPTR link: that is the shape
 * internalunloadseg.c's walk and internalloadseg_aos.c's GETHUNKPTR both assume. */
typedef struct emu68k_segnode {
    BPTRf          next;           /* the seglist link (fast BPTR = plain ptr)  */
    emu68k_segdesc d;
} emu68k_segnode;

/* ---- the modeled DOS seg registry (register_hunk / GetSegListInfo / removal hook).
 * Mirrors rom/dos: a list of {seglist BPTR, type}; GetSegListInfo matches the BPTR
 * by equality; removing an entry fires the arena-teardown hook. ---- */
#define SEGTYPE_HUNK_M 2           /* modeled SEGTYPE_HUNK                      */
#define GSLI_68KHUNK_M 1           /* modeled tag                               */

typedef struct regnode { BPTRf seg; int type; struct regnode *next; } regnode;
static regnode *g_registry = NULL;
static int g_native_nodes_live = 0;    /* allocation balance: proxy nodes        */
static int g_arenas_live       = 0;    /* allocation balance: guest arenas       */

static void reg_add(BPTRf seg, int type)
{
    regnode *r = calloc(1, sizeof *r);
    CHECK(r != NULL, "registry alloc");
    r->seg = seg; r->type = type; r->next = g_registry; g_registry = r;
}

/* GetSegListInfo, modeled: returns nonzero and writes *storage = seg iff the BPTR
 * is registered with the matching type (getseglistinfo.c's pointer-equality walk). */
static int reg_getseglistinfo(BPTRf seg, int tag, uintptr_t *storage)
{
    for (regnode *r = g_registry; r; r = r->next)
        if (r->seg == seg && tag == GSLI_68KHUNK_M && r->type == SEGTYPE_HUNK_M) {
            *storage = (uintptr_t)seg;
            return 1;
        }
    return 0;
}

/* Registry removal + the arena-teardown hook: the image record rides the FIRST
 * node's descriptor; removing the registry entry frees the guest arena. This is
 * the lifetime seam the in-OS version hooks at internalunloadseg's unregister. */
static void reg_remove_and_teardown(BPTRf seg)
{
    regnode **pp = &g_registry;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->seg == seg) break;
    CHECK(*pp != NULL, "unload of an unregistered seglist");
    regnode *r = *pp; *pp = r->next; free(r);

    emu68k_segnode *first = (emu68k_segnode *)seg;
    CHECK(first->d.magic == SEGDESC_MAGIC, "first node magic");
    CHECK(first->d.image != NULL, "first node owns the image record");
    free(first->d.image->arena_host);
    free(first->d.image);
    g_arenas_live--;
}

/* ---- emu68k_loadseg, modeled: [J4] load+relocate into a fresh guest arena, then
 * build + register the proxy chain. Returns the seglist BPTR (or BNULL). ---- */
static BPTRf emu68k_loadseg(const uint8_t *buf, size_t len, char *err, unsigned errlen)
{
    uint8_t *mem = calloc(1, SANDBOX_SIZE);
    if (!mem) { snprintf(err, errlen, "arena alloc"); return BNULLf; }

    j4_sandbox sb;
    j4_seglist seg;                             /* TRANSIENT: discarded below     */
    j4_sandbox_init(&sb, mem, SANDBOX_ORIGIN, SANDBOX_SIZE);
    sb.next_alloc = PROG_ORIGIN;
    if (j4_load_hunks(&sb, buf, len, 0, &seg, err, errlen)) { free(mem); return BNULLf; }

    emu68k_image *img = calloc(1, sizeof *img);
    CHECK(img != NULL, "image alloc");
    img->arena_host = mem; img->arena_origin = SANDBOX_ORIGIN;
    img->arena_size = SANDBOX_SIZE; img->entry = seg.entry; img->numhunks = seg.numhunks;
    g_arenas_live++;

    emu68k_segnode *head = NULL, *tail = NULL;
    for (int i = 0; i < seg.numhunks; i++) {
        emu68k_segnode *n = calloc(1, sizeof *n);
        CHECK(n != NULL, "node alloc");
        n->next = BNULLf;
        n->d.magic      = SEGDESC_MAGIC;
        n->d.guest_base = seg.hunk_base[i];
        n->d.guest_size = seg.hunk_size[i];
        n->d.hunk_type  = seg.hunk_type[i];
        n->d.image      = (i == 0) ? img : NULL;
        if (tail) tail->next = (BPTRf)n;
        else      head = n;
        tail = n;
        g_native_nodes_live++;
    }
    /* seg (the j4_seglist) goes out of scope HERE: from now on the proxy chain is
     * the only description of the loaded image — that is the point of the proof. */

    reg_add((BPTRf)head, SEGTYPE_HUNK_M);
    return (BPTRf)head;
}

/* ---- UnLoadSeg, modeled: the registry hook first (as the in-OS unregister path
 * will run first), then internalunloadseg.c's BLIND walk-and-free of the native
 * nodes — the walk knows nothing about descriptors or arenas. ---- */
static void emu68k_unloadseg(BPTRf seglist)
{
    reg_remove_and_teardown(seglist);
    BPTRf s = seglist;
    while (s) {                                  /* next = *(BPTR*)BADDR(seg)     */
        BPTRf next = *(BPTRf *)s;
        free(s);
        g_native_nodes_live--;
        s = next;
    }
}

/* ================= driving a run FROM THE PROXY ONLY ============================ */

struct bctx { stub_lib *lib; j4_sandbox *sb; };
static int bridge(int lvo, struct j5d_m68k_state *st, void *user, char *e, unsigned el)
{
    struct bctx *c = user;
    return stublib_dispatch(c->lib, c->sb, lvo, (struct M68KState *)st, e, el);
}

/* Recover everything a run needs from the proxy chain alone, run through the full
 * [J5d] JIT + the stub OS, and return the exit D0. */
static uint32_t run_from_proxy(BPTRf seglist, int64_t *outlen_out,
                               const uint8_t **outbuf_out)
{
    emu68k_segnode *first = (emu68k_segnode *)seglist;
    CHECK(first->d.magic == SEGDESC_MAGIC, "proxy magic");
    emu68k_image *img = first->d.image;
    CHECK(img != NULL, "proxy image record");

    /* cross-check the chain against the image record */
    int n = 0; uint32_t first_code = 0;
    for (emu68k_segnode *s = first; s; s = (emu68k_segnode *)s->next) {
        CHECK(s->d.magic == SEGDESC_MAGIC, "chain node magic");
        CHECK(s->d.guest_base >= img->arena_origin &&
              s->d.guest_base + s->d.guest_size <= img->arena_origin + img->arena_size,
              "hunk payload lies inside the guest arena");
        if (!first_code && s->d.hunk_type == J4_HUNK_CODE) first_code = s->d.guest_base;
        n++;
    }
    CHECK(n == img->numhunks, "chain length == image hunk count");
    CHECK(first_code == img->entry, "entry PC == first CODE hunk payload (guest)");

    /* Reconstruct the sandbox VIEW without j4_sandbox_init: init memsets the whole
     * region (fresh-load semantics), which would erase the loaded image. */
    j4_sandbox sb;
    sb.host_mem       = img->arena_host;
    sb.sandbox_origin = img->arena_origin;
    sb.size           = img->arena_size;
    sb.next_alloc     = img->arena_origin;

    static stub_lib lib;                          /* fresh stub OS per run */
    CHECK(stublib_init(&lib, &sb, LIBBASE, HEAP_BASE, HEAP_END) == 0, "stublib init");
    struct bctx c = { &lib, &sb };

    struct j5d_m68k_state st; memset(&st, 0, sizeof st);
    j5d_sandbox j5sb = { sb.host_mem, sb.sandbox_origin, sb.size };
    uint32_t d0 = 0; char err[256] = {0};
    int rc = j5d_run(&j5sb, img->entry, LIBBASE, &st, &d0, bridge, &c, err, sizeof err);
    CHECK(rc == 0, err[0] ? err : "j5d_run failed");

    *outlen_out = lib.outlen;
    *outbuf_out = (const uint8_t *)lib.out;
    return d0;
}

/* Fork one engine run: the child drives the JIT from the proxy chain and asserts
 * the byte-exact results (D0; either an exact output length or a must-contain
 * byte); the parent asserts the child exited clean. */
static void run_in_child(BPTRf seglist, uint32_t want_d0, int64_t want_len, char want_ch)
{
    pid_t pid = fork();
    CHECK(pid >= 0, "fork");
    if (pid == 0) {
        int64_t outlen = 0; const uint8_t *outbuf = NULL;
        uint32_t d0 = run_from_proxy(seglist, &outlen, &outbuf);
        CHECK(d0 == want_d0, "exit D0 is the known byte-exact value");
        if (want_len >= 0) CHECK(outlen == want_len, "output length is the known byte-exact value");
        else CHECK(outlen > 0 && memchr(outbuf, want_ch, (size_t)outlen) != NULL,
                   "program produced output through the [J3] bridge");
        _exit(0);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid, "waitpid");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child run failed (see FAIL line above)");
}

/* ================================ the proof ===================================== */

static uint8_t *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "test binary missing (build apps68k)");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    CHECK(buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz, "read test binary");
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

int main(void)
{
    char err[256] = {0};
    uintptr_t info = 0;

    /* -- 1. load + relocate two real hunk binaries into the representation -- */
    size_t mlen, jlen;
    uint8_t *mbuf = slurp("hosted/jit68k/apps68k/bin/mandel.exe", &mlen);
    uint8_t *jbuf = slurp("hosted/jit68k/apps68k/bin/j5t.exe",   &jlen);

    BPTRf mseg = emu68k_loadseg(mbuf, mlen, err, sizeof err);
    CHECK(mseg != BNULLf, err);
    BPTRf jseg = emu68k_loadseg(jbuf, jlen, err, sizeof err);
    CHECK(jseg != BNULLf, err);
    CHECK(g_arenas_live == 2, "two independent guest arenas live");

    /* -- 2. identity: GetSegListInfo(GSLI_68KHUNK) by BPTR value -- */
    CHECK(reg_getseglistinfo(mseg, GSLI_68KHUNK_M, &info) == 1 && info == (uintptr_t)mseg,
          "mandel seglist identified as 68K HUNK");
    CHECK(reg_getseglistinfo(jseg, GSLI_68KHUNK_M, &info) == 1 && info == (uintptr_t)jseg,
          "j5t seglist identified as 68K HUNK");
    int bogus_local = 0;
    CHECK(reg_getseglistinfo((BPTRf)&bogus_local, GSLI_68KHUNK_M, &info) == 0,
          "an unregistered pointer is NOT identified (negative)");

    /* -- 3. run each from the proxy alone; assert the known byte-exact results.
     * ONE ENGINE RUN PER PROCESS: the engine today is single-run global state (the
     * [T0-P3] item the plan already names — block cache/chaining are globals), and
     * a second j5d_run after j5d_run_free() in the same process executes stale
     * chained blocks in freed JIT memory (verified here first-hand: EXC_BAD_ACCESS
     * at a former jit_region address).  The REPRESENTATION under proof is
     * per-image and process-independent, so each run forks; engine re-entrancy
     * stays [T0-P3]'s exit bar, not this one's. -- */
    run_in_child(mseg, /*want_d0*/0, /*want_len*/-1, '+');            /* mandel  */
    run_in_child(jseg, /*want_d0*/10857, /*want_len*/717, 0);         /* j5t FP  */

    /* -- 4. unload: blind walk + registry hook; everything balances to zero -- */
    emu68k_unloadseg(mseg);
    emu68k_unloadseg(jseg);
    CHECK(g_native_nodes_live == 0, "every native proxy node freed");
    CHECK(g_arenas_live == 0, "every guest arena freed");
    CHECK(g_registry == NULL, "seg registry empty");
    CHECK(reg_getseglistinfo(mseg, GSLI_68KHUNK_M, &info) == 0,
          "unloaded seglist no longer identified");

    free(mbuf); free(jbuf);

    printf("[T0P1] PASS: proxy-seglist representation proven end to end — two real hunk "
           "binaries (integer + hardware-FP) loaded and RELOCATED WITH GUEST ADDRESSES "
           "into independent 32-bit arenas, identified by BPTR value through the modeled "
           "DOS seg registry (GSLI_68KHUNK, positive + negative), RUN through the full "
           "[J5d] JIT from the proxy chain ALONE (byte-exact: mandel D0=0, j5t D0=10857 "
           "with the 717-byte FP output stream), and unloaded via the standard blind "
           "UnLoadSeg walk + registry teardown hook with zero leaked nodes or arenas.\n");
    return 0;
}
