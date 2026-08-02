/* j5v_conform.c - the translator conformance runner.
 *
 * Runs every generated case through BOTH engines and compares the final state.
 * The interpreter decodes each addressing mode in plain C, with no register
 * allocator and no emission layer, so where the two disagree the JIT is what
 * is wrong.
 *
 * Deliberately NOT the lockstep differ: that compares at block boundaries,
 * where the two engines legitimately line up at transiently different points
 * on a multi-block program (see run68k.c). Comparing the FINAL state of a
 * small program has no such ambiguity - the program either computed the same
 * thing or it did not.
 *
 * Marker: [J5V] PASS / FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>

#include "j4_hunk.h"
#include "j5d_jit68k.h"

/* The engine CACHES translated blocks keyed on GUEST address, and every case
 * here loads a different program at the same address. Without a fresh cache,
 * case N runs case N-1's translated code - which is exactly why the first case
 * passed and every later one reported an impossible terminator.
 *
 * Freeing the cache in place is not the answer (chained blocks still point into
 * the regions just unmapped, which is a SIGILL, and is what the engine-instance
 * work was introduced to replace). Each case gets its own INSTANCE instead. */

#define ORIGIN   0x00210000u
#define ARENA    (4u * 1024u * 1024u)
#define PROG     0x00250000u

extern uint16_t j5d_pack_sr(const struct j5d_m68k_state *st);
extern int j5d_interp_run(j5d_sandbox *sb, uint32_t entry_pc, uint32_t a6_libbase,
                          struct j5d_m68k_state *st, uint32_t *exit_d0,
                          j5d_lvo_fn lvo, void *user, char *errbuf, unsigned errlen);

struct outcome
{
    int      rc;
    uint32_t d0;
    struct j5d_m68k_state st;
    uint8_t  mem[64];            /* the scratch area the case writes */
    char     err[192];
};

/* Load `path` into a fresh arena and run it through `run`. */
static int run_one(const char *path, struct outcome *o,
                   int (*run)(j5d_sandbox *, uint32_t, uint32_t,
                              struct j5d_m68k_state *, uint32_t *,
                              j5d_lvo_fn, void *, char *, unsigned))
{
    uint8_t *arena = calloc(1, ARENA);
    j4_sandbox sb;
    j4_seglist seg;
    long n;
    uint8_t *img;
    FILE *f;

    memset(o, 0, sizeof *o);
    if (!arena) return -1;

    f = fopen(path, "rb");
    if (!f) { free(arena); return -1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    img = malloc(n ? n : 1);
    if (!img || fread(img, 1, n, f) != (size_t)n) { fclose(f); free(img); free(arena); return -1; }
    fclose(f);

    j4_sandbox_init(&sb, arena, ORIGIN, ARENA);
    sb.next_alloc = PROG;
    if (j4_load_hunks(&sb, img, n, 0, &seg, o->err, sizeof o->err))
    { free(img); free(arena); return -1; }

    o->st.a[7] = ORIGIN + ARENA - 256;          /* a stack inside the arena */
    {
        j5d_sandbox j5sb = { sb.host_mem, sb.sandbox_origin, sb.size };
        if (run == j5d_run)                     /* only the JIT caches blocks */
        {
            j5d_engine *e = j5d_engine_new();   /* this case's own cache      */
            j5d_engine_activate(e);
            o->rc = run(&j5sb, seg.entry, 0, &o->st, &o->d0, NULL, NULL,
                        o->err, sizeof o->err);
            j5d_engine_activate(NULL);
            j5d_engine_free(e);
        }
        else
            o->rc = run(&j5sb, seg.entry, 0, &o->st, &o->d0, NULL, NULL,
                        o->err, sizeof o->err);
    }
    /* the case writes its trace just past its own code; copy a window of the
     * arena so a wrong value that happens to match in a register still shows */
    memcpy(o->mem, arena + (PROG - ORIGIN) + 0x40, sizeof o->mem);

    free(img);
    free(arena);
    return 0;
}

static int compare(const char *name, struct outcome *a, struct outcome *b)
{
    int bad = 0, i;

    if (a->rc != b->rc)
    {
        printf("  %-22s rc: jit=%d (%s) interp=%d (%s)\n", name,
               a->rc, a->err[0] ? a->err : "-", b->rc, b->err[0] ? b->err : "-");
        return 1;
    }
    if (a->rc != 0)                              /* both refused it the same way */
        return 0;

    for (i = 0; i < 8; i++)
        if (a->st.d[i] != b->st.d[i])
        { printf("  %-22s D%d: jit=%08x interp=%08x\n", name, i,
                 a->st.d[i], b->st.d[i]); bad = 1; }
    for (i = 0; i < 7; i++)                      /* A7 is the stack: not compared */
        if (a->st.a[i] != b->st.a[i])
        { printf("  %-22s A%d: jit=%08x interp=%08x\n", name, i,
                 a->st.a[i], b->st.a[i]); bad = 1; }
    if (j5d_pack_sr(&a->st) != j5d_pack_sr(&b->st))
    { printf("  %-22s SR: jit=%04x interp=%04x\n", name,
             j5d_pack_sr(&a->st), j5d_pack_sr(&b->st)); bad = 1; }
    if (memcmp(a->mem, b->mem, sizeof a->mem))
    { printf("  %-22s memory trace differs\n", name); bad = 1; }
    return bad;
}

static int by_name(const void *x, const void *y)
{
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build/conform";
    /* unbuffered: if a case takes the process down, the last line printed has
     * to be the case that did it */
    setvbuf(stdout, NULL, _IONBF, 0);
    char *names[512];
    int count = 0, failed = 0, unchecked = 0, i;
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d) { printf("[J5V] SKIP: no case directory %s\n", dir); return 0; }
    while ((e = readdir(d)) && count < 512)
    {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".exe"))
            names[count++] = strdup(e->d_name);
    }
    closedir(d);
    if (!count) { printf("[J5V] SKIP: no cases in %s\n", dir); return 0; }
    qsort(names, count, sizeof names[0], by_name);

    for (i = 0; i < count; i++)
    {
        char path[1024];
        struct outcome jit, ref;
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (getenv("J5V_TRACE")) printf("  .. %s\n", names[i]);
        if (run_one(path, &jit, j5d_run) || run_one(path, &ref, j5d_interp_run))
        { printf("  %-22s could not be run\n", names[i]); failed++; continue; }
        /* The oracle is a SUBSET interpreter. Where it cannot decode something,
         * it has no opinion, and that is a gap in the oracle rather than a
         * translator bug - so it is counted and named, never passed off as
         * agreement. */
        if (ref.rc != 0 && (strstr(ref.err, "out-of-subset") ||
                            strstr(ref.err, "oracle")))
        {
            printf("  %-22s UNCHECKED: the oracle does not decode this mode\n",
                   names[i]);
            unchecked++;
            continue;
        }
        failed += compare(names[i], &jit, &ref) ? 1 : 0;
    }

    if (failed)
    {
        printf("[J5V] FAIL: %d of %d instruction/addressing-mode cases disagree "
               "between the JIT and the interpreter oracle (%d unchecked).\n",
               failed, count, unchecked);
        return 1;
    }
    printf("[J5V] PASS: %d of %d instruction/addressing-mode cases agree exactly "
           "between the JIT and the interpreter oracle (registers and memory)"
           "%s.\n", count - unchecked, count,
           unchecked ? "; the rest are modes the oracle cannot decode, listed above"
                     : "");
    return 0;
}
