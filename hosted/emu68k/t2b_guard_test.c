/* t2b_guard_test.c — [T2b] the runtime hardware guard (OURS, AROS-licensed).
 *
 * The static scan predicts; the guard decides. A guest touch of the Amiga
 * hardware must come back as a classified ROUTING EVENT naming the register -
 * not a crash, not silent corruption - including the case no scanner can
 * predict, where the address is computed at run time.
 * Marker: [T2B] PASS / FAIL. */

#include "emu68k_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#define CHECK(c, why) do { if (!(c)) { \
    fprintf(stderr, "[T2B] FAIL: %s (line %d)\n", why, __LINE__); exit(1); } } while (0)

static emu68k_run *(*p_new)(const void *, unsigned long, const char *, unsigned long,
                            emu68k_sink_fn, void *, char *, unsigned);
static int  (*p_quantum)(emu68k_run *, unsigned long, unsigned int *, char *, unsigned);
static void (*p_free)(emu68k_run *);

static int run_prog(const char *name, char *err, unsigned el)
{
    char path[256];
    snprintf(path, sizeof path, "hosted/emu68k/scantests/bin/%s.exe", name);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "test program missing (make hosted-emu68k-t2scan builds them)");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    CHECK(b && fread(b, 1, (size_t)n, f) == (size_t)n, "read test program");
    fclose(f);

    err[0] = 0;
    emu68k_run *r = p_new(b, (unsigned long)n, "", 0, NULL, NULL, err, el);
    CHECK(r != NULL, err);
    unsigned int d0 = 0; int rc;
    while ((rc = p_quantum(r, 4096, &d0, err, el)) == EMU68K_RC_YIELD) { }
    p_free(r); free(b);
    return rc;
}

int main(int argc, char **argv)
{
    const char *lib = (argc > 1) ? argv[1] : "build/libemu68k.dylib";
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "[T2B] FAIL: dlopen: %s\n", dlerror()); return 1; }
    p_new     = dlsym(h, "emu68k_run_new");
    p_quantum = dlsym(h, "emu68k_run_quantum");
    p_free    = dlsym(h, "emu68k_run_free");
    CHECK(p_new && p_quantum && p_free, "dlsym");

    char err[256];
    struct { const char *prog, *want; } hw[] = {
        { "chipbang",   "custom chip register $DFF182" },
        { "ciapeek",    "CIA register $BFE001"         },  /* absolute address    */
        { "ciapeek_an", "CIA register $BFE001"         },  /* through a register  */
        { "computedhw", "custom chip register $DFF180" },  /* computed at runtime */
        { "vecwrite",   "exception vector page $068"   },
    };
    for (unsigned i = 0; i < sizeof hw / sizeof hw[0]; i++) {
        int rc = run_prog(hw[i].prog, err, sizeof err);
        if (rc != EMU68K_RC_HARDWARE || !strstr(err, hw[i].want)) {
            fprintf(stderr, "[T2B] FAIL: %s -> rc=%d err=\"%s\" (wanted a hardware "
                            "event naming \"%s\")\n", hw[i].prog, rc, err, hw[i].want);
            return 1;
        }
        fprintf(stderr, "  ok   %-11s hardware event: %s\n", hw[i].prog, err);
    }

    /* the negative controls must run to completion, untouched by the guard */
    const char *ok[] = { "datadecoy", "opdecoy", "color00" };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        int rc = run_prog(ok[i], err, sizeof err);
        if (rc != EMU68K_RC_DONE) {
            fprintf(stderr, "[T2B] FAIL: %s -> rc=%d err=\"%s\" (wanted a clean run)\n",
                    ok[i], rc, err);
            return 1;
        }
        fprintf(stderr, "  ok   %-11s ran clean\n", ok[i]);
    }

    printf("[T2B] PASS: every unsupported hardware touch comes back as a classified routing event "
           "naming the exact register - custom chips and CIAs, reached absolutely or "
           "through an address register, INCLUDING an address computed at run time that "
           "no static scan can predict - while the negative controls and the exact "
           "COLOR00 calibration write run untouched.\n");
    return 0;
}
