/* Focused classic Workbench startup mirror test. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "emu68k_host.h"

int main(void)
{
    void *h = dlopen("build/libemu68k.dylib", RTLD_NOW);
    emu68k_run *(*run_new)(const void *, unsigned long, const char *,
                           unsigned long, emu68k_sink_fn, void *, char *,
                           unsigned);
    int (*set_workbench)(emu68k_run *, unsigned long, const unsigned int *,
                         const char *const *, char *, unsigned);
    int (*run_quantum)(emu68k_run *, unsigned long, unsigned int *, char *,
                       unsigned);
    void (*run_free)(emu68k_run *);
    const unsigned int locks[] = { 0x11111111u, 0x22222222u };
    const char *const names[] = { "Wordworth", "Project" };
    unsigned char *image;
    unsigned int result = 0;
    char err[256] = {0};
    emu68k_run *run;
    FILE *f;
    long len;
    int rc;

    if (!h) {
        fprintf(stderr, "[T3WORKBENCH] FAIL: %s\n", dlerror());
        return 1;
    }
    run_new = dlsym(h, "emu68k_run_new");
    set_workbench = dlsym(h, "emu68k_run_set_workbench");
    run_quantum = dlsym(h, "emu68k_run_quantum");
    run_free = dlsym(h, "emu68k_run_free");
    if (!run_new || !set_workbench || !run_quantum || !run_free) {
        fprintf(stderr, "[T3WORKBENCH] FAIL: incomplete host API\n");
        return 1;
    }
    f = fopen("build/workbench-startup.exe", "rb");
    if (!f) {
        fprintf(stderr, "[T3WORKBENCH] FAIL: missing fixture\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    image = malloc((size_t)len);
    if (len <= 0 || !image || fread(image, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(image);
        fprintf(stderr, "[T3WORKBENCH] FAIL: could not read fixture\n");
        return 1;
    }
    fclose(f);
    run = run_new(image, (unsigned long)len, "", 0, NULL, NULL, err,
                  sizeof err);
    free(image);
    if (!run || set_workbench(run, 2, locks, names, err, sizeof err) != 0) {
        fprintf(stderr, "[T3WORKBENCH] FAIL: %s\n", err);
        if (run) run_free(run);
        return 1;
    }
    do {
        rc = run_quantum(run, 4096, &result, err, sizeof err);
    } while (rc == EMU68K_RC_YIELD);
    run_free(run);
    if (rc != EMU68K_RC_DONE || result != 0) {
        fprintf(stderr, "[T3WORKBENCH] FAIL: rc=%d result=%u %s\n",
                rc, result, err);
        return 1;
    }
    puts("[T3WORKBENCH] PASS: pr_CLI, Process port, WBStartup, locks and "
         "argument names match the classic 68k contract.");
    return 0;
}
