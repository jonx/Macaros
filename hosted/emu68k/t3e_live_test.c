/* [T3e] Drive a real program through the live libemu68k OpenLibrary seam. */
#include "emu68k_host.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct regs68k { unsigned d[8], a[8]; };
static FILE *handles[8];

/* Stand in for the installed AROS DOS bridge. This deliberately accepts only
 * LIBS:/PROGDIR: paths, so the regression proves the live resolver used DOS
 * semantics rather than falling through to EMU68K_LIBS_PATH. */
static int oscall(const char *lib, int lvo, void *regs, void *guest0,
                  void *user, char *err, unsigned errlen)
{
    struct regs68k *r = regs;
    unsigned char *g = guest0;
    (void)user;
    if (strcmp(lib, "dos.library")) { snprintf(err, errlen, "not dos"); return 1; }
    if (lvo == 5) {
        const char *name = (const char *)(g + r->d[1]);
        const char *leaf = !strncmp(name, "LIBS:", 5) ? name + 5 :
                           !strncmp(name, "PROGDIR:", 8) ? name + 8 : NULL;
        char path[512];
        int i;
        if (!leaf) { r->d[0] = 0; return 0; }
        snprintf(path, sizeof path, "build/emu68k-nativelib/%s", leaf);
        for (i = 1; i < 8 && handles[i]; i++) {}
        if (i == 8 || !(handles[i] = fopen(path, "rb"))) r->d[0] = 0;
        else r->d[0] = (unsigned)i;
        return 0;
    }
    if (r->d[1] >= 8 || !handles[r->d[1]]) { r->d[0] = 0xffffffffu; return 0; }
    if (lvo == 6) {
        fclose(handles[r->d[1]]); handles[r->d[1]] = NULL; r->d[0] = 1; return 0;
    }
    if (lvo == 7) {
        r->d[0] = (unsigned)fread(g + r->d[2], 1, r->d[3], handles[r->d[1]]);
        return 0;
    }
    if (lvo == 11) {
        FILE *f = handles[r->d[1]];
        long old = ftell(f);
        int whence = r->d[3] == 0xffffffffu ? SEEK_SET :
                     r->d[3] == 0 ? SEEK_CUR : SEEK_END;
        r->d[0] = fseek(f, (long)(int)r->d[2], whence) ? 0xffffffffu : (unsigned)old;
        return 0;
    }
    snprintf(err, errlen, "dos LVO %d unsupported", lvo);
    return 1;
}

int main(int argc, char **argv)
{
    void *h;
    FILE *f;
    unsigned char *image;
    long n;
    char err[256] = {0};
    unsigned d0 = 0;
    int rc;
    emu68k_run *run;
    emu68k_run *(*run_new)(const void *, unsigned long, const char *, unsigned long,
                           emu68k_sink_fn, void *, char *, unsigned);
    int (*run_quantum)(emu68k_run *, unsigned long, unsigned *, char *, unsigned);
    void (*run_free)(emu68k_run *);
    void (*set_oscall)(emu68k_oscall_fn, void *);

    if (argc != 2) { fprintf(stderr, "usage: %s guestopen.exe\n", argv[0]); return 2; }
    h = dlopen("build/libemu68k.dylib", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    run_new = dlsym(h, "emu68k_run_new");
    run_quantum = dlsym(h, "emu68k_run_quantum");
    run_free = dlsym(h, "emu68k_run_free");
    set_oscall = dlsym(h, "emu68k_set_oscall");
    if (!run_new || !run_quantum || !run_free || !set_oscall) return 1;
    set_oscall(oscall, NULL);

    f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    image = malloc((size_t)n);
    if (!image || fread(image, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);

    run = run_new(image, (unsigned long)n, "", 0, NULL, NULL, err, sizeof err);
    free(image);
    if (!run) { fprintf(stderr, "[T3E-LIVE] FAIL: load: %s\n", err); return 1; }
    while ((rc = run_quantum(run, 4096, &d0, err, sizeof err)) == EMU68K_RC_YIELD) {}
    run_free(run);
    if (rc != EMU68K_RC_DONE || d0 != 0) {
        fprintf(stderr, "[T3E-LIVE] FAIL: rc=%d d0=%u %s\n", rc, d0, err);
        return 1;
    }
    printf("[T3E-LIVE] PASS: live exec.OpenLibrary ran AUTOINIT and direct-init disk "
           "libraries; Close/Expunge unregistered before reload; version failure and "
           "A->B->A dependency cycle rolled back normally; later opens still worked; "
           "a per-opener (clone) base library gave two live openers distinct working "
           "bases, closed each on its own, and expunged on the last\n");
    return 0;
}
