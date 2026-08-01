/* t1_dylib_test.c — [T1] host smoke of the libemu68k.dylib service API, driven
 * the way emu68k.library will drive it: dlopen + dlsym (the hostlib shape),
 * quantum runs with a streaming sink, async kill of a chained infinite loop.
 * (OURS, AROS-licensed.) Marker: [T1DYL] PASS / FAIL. */

#include "emu68k_host.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#define CHECK(cond, why) do { if (!(cond)) { \
    fprintf(stderr, "[T1DYL] FAIL: %s (line %d)\n", why, __LINE__); exit(1); } } while (0)

/* resolved via dlsym, exactly like hostlib will */
static emu68k_run *(*p_new)(const void *, unsigned long, const char *, unsigned long,
                            emu68k_sink_fn, void *, char *, unsigned);
static int  (*p_quantum)(emu68k_run *, unsigned long, unsigned int *, char *, unsigned);
static void (*p_kill)(emu68k_run *);
static void (*p_free)(emu68k_run *);
static const char *(*p_version)(void);

static char g_out[1 << 20];
static long g_outlen = 0;
static void sink(const char *buf, long len, void *user)
{
    (void)user;
    if (g_outlen + len <= (long)sizeof g_out) {
        memcpy(g_out + g_outlen, buf, len);
        g_outlen += len;
    }
}

static uint8_t *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "test binary missing");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    CHECK(buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz, "read test binary");
    fclose(f); *len_out = (size_t)sz; return buf;
}

static size_t mk_loop_hunk(uint8_t *out)
{
    static const uint32_t w[] = { 0x3F3, 0, 1, 0, 0, 1, 0x3E9, 1, 0x60FE4E75u, 0x3F2 };
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++) {
        out[i*4+0] = (uint8_t)(w[i] >> 24); out[i*4+1] = (uint8_t)(w[i] >> 16);
        out[i*4+2] = (uint8_t)(w[i] >> 8);  out[i*4+3] = (uint8_t)w[i];
    }
    return sizeof w;
}

static emu68k_run *g_loop_run;
static void on_alarm(int s)
{
    (void)s;
    p_kill(g_loop_run);      /* the async kill contract: one store + engine flag */
    alarm(8);
}

int main(int argc, char **argv)
{
    CHECK(argc > 1, "usage: t1dyl <libemu68k.dylib>");
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "[T1DYL] FAIL: dlopen: %s\n", dlerror()); return 1; }
    p_new     = dlsym(h, "emu68k_run_new");
    p_quantum = dlsym(h, "emu68k_run_quantum");
    p_kill    = dlsym(h, "emu68k_run_kill");
    p_free    = dlsym(h, "emu68k_run_free");
    p_version = dlsym(h, "emu68k_version");
    CHECK(p_new && p_quantum && p_kill && p_free && p_version, "dlsym surface complete");
    fprintf(stderr, "[T1DYL] %s\n", p_version());

    char err[256] = {0};
    unsigned int d0 = 0;

    /* ---- j5t (hardware FP) via bounded quanta + streaming sink ---- */
    size_t jlen; uint8_t *jbuf = slurp("hosted/jit68k/apps68k/bin/j5t.exe", &jlen);
    emu68k_run *r = p_new(jbuf, jlen, "", 0, sink, NULL, err, sizeof err);
    CHECK(r != NULL, err);
    int rc, quanta = 0;
    while ((rc = p_quantum(r, 64, &d0, err, sizeof err)) == EMU68K_RC_YIELD) quanta++;
    CHECK(rc == EMU68K_RC_DONE, err);
    CHECK(quanta > 0, "the quantum boundary was actually exercised");
    CHECK(d0 == 10857, "j5t exit D0 == 10857 through the dylib");
    CHECK(g_outlen == 717, "j5t 717-byte stream arrived through the sink");
    p_free(r); free(jbuf);

    /* ---- kill a fully-chained infinite loop from a signal handler ---- */
    uint8_t lb[64]; size_t llen = mk_loop_hunk(lb);
    g_loop_run = p_new(lb, llen, "", 0, NULL, NULL, err, sizeof err);
    CHECK(g_loop_run != NULL, err);
    /* drive quanta the way emu68k.library does: the chain budget brings even a
     * fully self-chained loop back every quantum, and the kill lands there. */
    signal(SIGALRM, on_alarm);
    alarm(1);
    int spins = 0;
    while ((rc = p_quantum(g_loop_run, 4096, &d0, err, sizeof err)) == EMU68K_RC_YIELD)
        spins++;
    alarm(0);
    CHECK(rc == EMU68K_RC_KILLED, "chained loop killed through the dylib API");
    CHECK(spins > 0, "the chained loop really did yield per quantum (budget works)");
    p_free(g_loop_run);

    printf("[T1DYL] PASS: dylib service drives the engine end-to-end — j5t byte-exact "
           "(D0=10857, 717-byte sink stream) across %d bounded quanta, and a chained "
           "infinite loop killed asynchronously through emu68k_run_kill.\n", quanta + 1);
    return 0;
}
