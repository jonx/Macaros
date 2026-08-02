/* [T3e] Real-software proof: Aminet xpk_User 5.2a's xQuery drives the
 * package's xpkmaster.library and xpkNONE.library through the live loader.
 * Third-party files are supplied by the caller and are never vendored. */
#include "emu68k_host.h"

#include <dlfcn.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

struct regs68k { uint32_t d[8], a[8]; };

struct real_ctx {
    const char *root;
    FILE *handles[32];
    struct { DIR *dir; char path[PATH_MAX]; } locks[16];
    char output[16384];
    size_t outlen;
};

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int fill_fib(uint8_t *g, uint32_t fib, const char *name,
                    int is_dir, uint32_t size)
{
    uint8_t *p = g + fib;
    size_t n = strlen(name);
    if (n >= 108u) n = 107u;
    memset(p, 0, 260);
    put_be32(p + 4, is_dir ? 2u : 0xffffffffu);      /* fib_DirEntryType */
    memcpy(p + 8, name, n);                          /* fib_FileName */
    put_be32(p + 120, is_dir ? 2u : 0xffffffffu);    /* fib_EntryType */
    put_be32(p + 124, size);                         /* fib_Size */
    put_be32(p + 128, (size + 511u) / 512u);         /* fib_NumBlocks */
    return 0;
}

static void append_output(struct real_ctx *c, const void *buf, size_t len)
{
    if (len > sizeof c->output - 1u - c->outlen)
        len = sizeof c->output - 1u - c->outlen;
    memcpy(c->output + c->outlen, buf, len);
    c->outlen += len;
    c->output[c->outlen] = 0;
}

static void sink(const char *buf, long len, void *user)
{
    if (len > 0) append_output(user, buf, (size_t)len);
}

static int dos_path(struct real_ctx *c, const char *name, char *path, size_t cap)
{
    const char *leaf;
    if (!strncmp(name, "LIBS:", 5)) {
        leaf = name + 5;
        if (strstr(leaf, "..") || strchr(leaf, ':')) return 1;
        return snprintf(path, cap, "%s/Libs/%s", c->root, leaf) >= (int)cap;
    }
    if (!strncmp(name, "PROGDIR:", 8)) {
        leaf = name + 8;
        if (strstr(leaf, "..") || strchr(leaf, ':')) return 1;
        return snprintf(path, cap, "%s/C/%s", c->root, leaf) >= (int)cap;
    }
    return 1;
}

static int oscall(const char *lib, int lvo, void *regs, void *guest0,
                  void *user, char *err, unsigned errlen)
{
    struct real_ctx *c = user;
    struct regs68k *r = regs;
    uint8_t *g = guest0;

    if (getenv("EMU68K_TRACE_OSCALL"))
        fprintf(stderr, "[real-os] %s LVO %d d0=%08x d1=%08x d2=%08x d3=%08x\n",
                lib, lvo, r->d[0], r->d[1], r->d[2], r->d[3]);
    if (strcmp(lib, "dos.library")) {
        snprintf(err, errlen, "%s LVO %d is not supplied by the host proof", lib, lvo);
        return 1;
    }

    switch (lvo) {
    case 5: {                                               /* Open */
        const char *name = (const char *)(g + r->d[1]);
        char path[PATH_MAX];
        int i;
        if (dos_path(c, name, path, sizeof path)) { r->d[0] = 0; return 0; }
        for (i = 1; i < 32 && c->handles[i]; i++) {}
        if (i == 32 || !(c->handles[i] = fopen(path, "rb"))) r->d[0] = 0;
        else r->d[0] = (uint32_t)i;
        return 0;
    }
    case 6:                                                /* Close */
        if (r->d[1] < 32 && c->handles[r->d[1]]) {
            fclose(c->handles[r->d[1]]); c->handles[r->d[1]] = NULL; r->d[0] = 1;
        } else r->d[0] = 0;
        return 0;
    case 7:                                                /* Read */
        if (r->d[1] >= 32 || !c->handles[r->d[1]]) { r->d[0] = 0xffffffffu; return 0; }
        r->d[0] = (uint32_t)fread(g + r->d[2], 1, r->d[3], c->handles[r->d[1]]);
        return 0;
    case 8:                                                /* Write */
        append_output(c, g + r->d[2], r->d[3]); r->d[0] = r->d[3]; return 0;
    case 10:                                               /* Output */
        r->d[0] = 0x7fffffffu; return 0;
    case 11: {                                             /* Seek */
        FILE *f;
        long old;
        int whence;
        if (r->d[1] >= 32 || !(f = c->handles[r->d[1]])) { r->d[0] = 0xffffffffu; return 0; }
        old = ftell(f);
        whence = r->d[3] == 0xffffffffu ? SEEK_SET : r->d[3] == 0 ? SEEK_CUR : SEEK_END;
        r->d[0] = fseek(f, (long)(int32_t)r->d[2], whence) ? 0xffffffffu : (uint32_t)old;
        return 0;
    }
    case 14: {                                             /* Lock */
        const char *name = (const char *)(g + r->d[1]);
        char path[PATH_MAX];
        int i;
        if (dos_path(c, name, path, sizeof path)) { r->d[0] = 0; return 0; }
        for (i = 1; i < 16 && c->locks[i].dir; i++) {}
        if (i == 16 || !(c->locks[i].dir = opendir(path))) r->d[0] = 0;
        else {
            snprintf(c->locks[i].path, sizeof c->locks[i].path, "%s", path);
            r->d[0] = 0x100u + (uint32_t)i;
        }
        return 0;
    }
    case 15: {                                             /* UnLock */
        uint32_t i = r->d[1] - 0x100u;
        if (i < 16 && c->locks[i].dir) {
            closedir(c->locks[i].dir); c->locks[i].dir = NULL;
        }
        r->d[0] = 0; return 0;
    }
    case 17: {                                             /* Examine */
        uint32_t i = r->d[1] - 0x100u;
        if (i >= 16 || !c->locks[i].dir) { r->d[0] = 0; return 0; }
        rewinddir(c->locks[i].dir);
        fill_fib(g, r->d[2], "compressors", 1, 0);
        r->d[0] = 1; return 0;
    }
    case 18: {                                             /* ExNext */
        uint32_t i = r->d[1] - 0x100u;
        struct dirent *de;
        if (i >= 16 || !c->locks[i].dir) { r->d[0] = 0; return 0; }
        while ((de = readdir(c->locks[i].dir)) != NULL) {
            char path[PATH_MAX];
            struct stat st;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (snprintf(path, sizeof path, "%s/%s", c->locks[i].path, de->d_name) >=
                (int)sizeof path || stat(path, &st)) continue;
            fill_fib(g, r->d[2], de->d_name, S_ISDIR(st.st_mode), (uint32_t)st.st_size);
            r->d[0] = 1; return 0;
        }
        r->d[0] = 0; return 0;
    }
    default:
        snprintf(err, errlen, "dos.library LVO %d is not supplied by the host proof", lvo);
        return 1;
    }
}

static unsigned char *read_file(const char *path, unsigned long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    long n;
    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET))
        return NULL;
    p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f); *len = (unsigned long)n; return p;
}

int main(int argc, char **argv)
{
    typedef emu68k_run *(*run_new_fn)(const void *, unsigned long, const char *,
                                      unsigned long, emu68k_sink_fn, void *,
                                      char *, unsigned);
    typedef int (*run_quantum_fn)(emu68k_run *, unsigned long, unsigned *,
                                  char *, unsigned);
    typedef void (*run_free_fn)(emu68k_run *);
    typedef void (*set_oscall_fn)(emu68k_oscall_fn, void *);
    typedef void (*set_name_fn)(emu68k_run *, const char *);
    struct real_ctx ctx = {0};
    char program[PATH_MAX], err[256] = {0};
    unsigned char *image;
    unsigned long imagelen;
    unsigned d0 = 0;
    int rc;
    void *h;
    emu68k_run *run;

    if (argc != 2) { fprintf(stderr, "usage: %s xpk_User-directory\n", argv[0]); return 2; }
    ctx.root = argv[1];
    snprintf(program, sizeof program, "%s/C/xQuery", ctx.root);
    image = read_file(program, &imagelen);
    if (!image) { fprintf(stderr, "[T3E-REAL] FAIL: cannot read %s\n", program); return 1; }

    h = dlopen("build/libemu68k.dylib", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    run_new_fn p_new = (run_new_fn)dlsym(h, "emu68k_run_new");
    run_quantum_fn p_quantum = (run_quantum_fn)dlsym(h, "emu68k_run_quantum");
    run_free_fn p_free = (run_free_fn)dlsym(h, "emu68k_run_free");
    set_oscall_fn p_oscall = (set_oscall_fn)dlsym(h, "emu68k_set_oscall");
    set_name_fn p_set_name = (set_name_fn)dlsym(h, "emu68k_run_set_name");
    if (!p_new || !p_quantum || !p_free || !p_oscall || !p_set_name) return 1;
    p_oscall(oscall, &ctx);

    run = p_new(image, imagelen, "NONE", 4, sink, &ctx, err, sizeof err);
    free(image);
    if (!run) { fprintf(stderr, "[T3E-REAL] FAIL: load: %s\n", err); return 1; }
    p_set_name(run, program);
    while ((rc = p_quantum(run, 4096, &d0, err, sizeof err)) == EMU68K_RC_YIELD) {}
    p_free(run);
    for (int i = 1; i < 32; i++) if (ctx.handles[i]) fclose(ctx.handles[i]);
    for (int i = 1; i < 16; i++) if (ctx.locks[i].dir) closedir(ctx.locks[i].dir);

    if (rc != EMU68K_RC_DONE || d0 != 0 || !strstr(ctx.output, "Packer") ||
        !strstr(ctx.output, "NONE") || !strstr(ctx.output, "Name") ||
        !strstr(ctx.output, "Descr") || !strstr(ctx.output, "DefMode")) {
        fprintf(stderr, "[T3E-REAL] FAIL: rc=%d d0=%u %s\noutput:\n%s\n",
                rc, d0, err, ctx.output);
        return 1;
    }
    printf("%s", ctx.output);
    printf("[T3E-REAL] PASS: unmodified xQuery -> xpkmaster.library -> "
           "xpkNONE.library ran from the redistributable xpk_User 5.2a package\n");
    return 0;
}
