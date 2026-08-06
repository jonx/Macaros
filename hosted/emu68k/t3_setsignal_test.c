/* [T3] SetSignal must share real guest signal state with Wait and PutMsg. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu68k_host.h"

/* Model hosted AROS exporting a guest libc symbol ahead of a dlopened host
 * shim.  The test must prove the shim reads Darwin's environment, not merely
 * the ordinary libc environment of an un-interposed command-line process. */
char *getenv(const char *name)
{
    (void)name;
    return "guest-environment";
}

int main(void)
{
    void *h = dlopen("build/libemu68k.dylib", RTLD_NOW);
    emu68k_run *(*run_new)(const void *, unsigned long, const char *,
                           unsigned long, emu68k_sink_fn, void *, char *,
                           unsigned);
    int (*run_quantum)(emu68k_run *, unsigned long, unsigned int *, char *,
                       unsigned);
    void (*run_free)(emu68k_run *);
    const char *(*host_getenv)(const char *);
    FILE *f;
    unsigned char *image;
    long len;
    unsigned int result = 0;
    char err[256] = {0};
    emu68k_run *run;
    int rc;

    if (!h) {
        fprintf(stderr, "[T3SETSIGNAL] FAIL: %s\n", dlerror());
        return 1;
    }
    run_new = dlsym(h, "emu68k_run_new");
    run_quantum = dlsym(h, "emu68k_run_quantum");
    run_free = dlsym(h, "emu68k_run_free");
    host_getenv = dlsym(h, "emu68k_host_getenv");
    if (!run_new || !run_quantum || !run_free || !host_getenv) {
        fprintf(stderr, "[T3SETSIGNAL] FAIL: incomplete emu68k host API\n");
        return 1;
    }
    if (setenv("EMU68K_HOST_ENV_TEST", "visible", 1) != 0 ||
        !host_getenv("EMU68K_HOST_ENV_TEST") ||
        strcmp(host_getenv("EMU68K_HOST_ENV_TEST"), "visible") != 0) {
        fprintf(stderr, "[T3SETSIGNAL] FAIL: host environment lookup\n");
        return 1;
    }
    f = fopen("build/setsignal.exe", "rb");
    if (!f) {
        fprintf(stderr, "[T3SETSIGNAL] FAIL: missing fixture\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    image = malloc((size_t)len);
    if (len <= 0 || !image || fread(image, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(image);
        fprintf(stderr, "[T3SETSIGNAL] FAIL: could not read fixture\n");
        return 1;
    }
    fclose(f);
    run = run_new(image, (unsigned long)len, "", 0, NULL, NULL, err,
                  sizeof err);
    free(image);
    if (!run) {
        fprintf(stderr, "[T3SETSIGNAL] FAIL: %s\n", err);
        return 1;
    }
    do {
        rc = run_quantum(run, 4096, &result, err, sizeof err);
    } while (rc == EMU68K_RC_YIELD);
    run_free(run);
    if (rc != EMU68K_RC_DONE || result != 0) {
        fprintf(stderr, "[T3SETSIGNAL] FAIL: rc=%d result=%u %s\n", rc,
                result, err);
        return 1;
    }
    puts("[T3SETSIGNAL] PASS: host environment lookup bypassed interposition; "
         "SetSignal returned the prior value and updated only its masked guest "
         "signal bits.");
    return 0;
}
