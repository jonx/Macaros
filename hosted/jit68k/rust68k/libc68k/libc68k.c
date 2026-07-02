/* libc68k.c — the tiny m68k C runtime under run68k's stub-DOS (STD68K-PLAN.md
 * piece 3). Provides EXACTLY the extern "C" surface the Rust std aros pal uses
 * (library/std/src/sys/⁎/aros.rs), implemented over the stub-DOS LVO veneers
 * (lvo.s). Compiled with m68k-elf-gcc (GCC backend: no LLVM CCR bug), linked by
 * vlink into the same hunk as the Rust staticlib.
 *
 * Conventions shared with the host side (stublib.h): LVO results are >= 0 or
 * NEGATIVE NetBSD-numbered errno; stat records and GetTime fields are big-endian
 * u32s, which read natively here (the 68k IS big-endian). The malloc heap and the
 * layout constants below mirror run68k.c's sandbox map. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the veneers (lvo.s) ---- */
extern int32_t __lvo_open(uint32_t path, uint32_t flags, uint32_t mode);
extern int32_t __lvo_close(uint32_t fd);
extern int32_t __lvo_read(uint32_t fd, uint32_t buf, uint32_t len);
extern int32_t __lvo_write(uint32_t fd, uint32_t buf, uint32_t len);
extern int32_t __lvo_lseek(uint32_t fd, uint32_t off, uint32_t whence);
extern int32_t __lvo_delete(uint32_t path);
extern int32_t __lvo_mkdir(uint32_t path, uint32_t mode);
extern int32_t __lvo_rmdir(uint32_t path);
extern int32_t __lvo_stat(uint32_t path, uint32_t rec);
extern int32_t __lvo_fstat(uint32_t fd, uint32_t rec);
extern int32_t __lvo_gettime(uint32_t which, uint32_t out8);
extern int32_t __lvo_entropy(uint32_t buf, uint32_t len);

/* ---- mem intrinsics ----
 * Rust/LLVM lowers bulk copies to calls to these C symbols. compiler_builtins can
 * provide them (-Zbuild-std-features=compiler-builtins-mem), BUT the experimental
 * LLVM M68k backend MISCOMPILES compiler_builtins' memcpy (it fails to advance the
 * source pointer — copies the first byte across the whole destination), which
 * corrupts every String/CString/format buffer in std. So we provide correct ones
 * here, compiled by the mature gcc m68k backend, and build std WITHOUT that feature. */
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dd = d; const uint8_t *ss = s;
    if (dd == ss || n == 0) return d;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void *memset(void *d, int c, size_t n)
{
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = a, *pb = b;
    while (n--) { int diff = (int)*pa++ - (int)*pb++; if (diff) return diff; }
    return 0;
}
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/* ---- errno ---- */
static int32_t g_errno;
int32_t *__stdc_geterrnoptr(void) { return &g_errno; }

/* Fold an LVO result: negative -> set errno, return -1 (or NULL-ish); else pass. */
static int32_t fold(int32_t r)
{
    if (r < 0) { g_errno = -r; return -1; }
    return r;
}

const char *strerror(int32_t e)
{
    static char buf[24];
    char *p = buf;
    const char *s = "stub-dos error ";
    while (*s) *p++ = *s++;
    if (e >= 100) *p++ = (char)('0' + e / 100 % 10);
    if (e >= 10)  *p++ = (char)('0' + e / 10 % 10);
    *p++ = (char)('0' + e % 10);
    *p = 0;
    return buf;
}

/* ---- malloc: first-fit free list over the fixed heap region (run68k.c map) ----
 * Header = size|used in the low bit; blocks 8-aligned. Single task, no locking. */
#define HEAP_LO 0x00C00000u
#define HEAP_HI 0x01100000u
typedef struct blk { uint32_t sz; struct blk *next; } blk;   /* free-list node */
static blk *g_free;
static int  g_heap_up;

static void heap_init(void)
{
    g_free = (blk *)(uintptr_t)HEAP_LO;
    g_free->sz = HEAP_HI - HEAP_LO;
    g_free->next = 0;
    g_heap_up = 1;
}

void *malloc(size_t n)
{
    if (!g_heap_up) heap_init();
    if (n == 0) n = 1;
    uint32_t need = ((uint32_t)n + 7u & ~7u) + 8u;           /* payload + header */
    blk **pp = &g_free;
    for (blk *b = g_free; b; pp = &b->next, b = b->next) {
        if (b->sz < need) continue;
        if (b->sz >= need + 24u) {                            /* split */
            blk *rest = (blk *)((uint8_t *)b + need);
            rest->sz = b->sz - need;
            rest->next = b->next;
            *pp = rest;
            b->sz = need;
        } else {
            *pp = b->next;
        }
        *(uint32_t *)b = b->sz | 1u;
        return (uint8_t *)b + 8;
    }
    g_errno = 12; /* ENOMEM */
    return 0;
}

/* Aligned blocks (posix_memalign) carry a MAGIC + the raw malloc pointer just
 * before the aligned address, so plain free() (what Rust's dealloc calls) can
 * route them back. A normal used-block header always has bit 0 SET (size|1);
 * the magic has bit 0 clear, so the two cannot be confused. */
#define ALIGN_MAGIC 0xA11A11A0u

void free(void *p)
{
    if (!p) return;
    uint32_t tag = ((uint32_t *)p)[-2];
    if (tag == ALIGN_MAGIC) {                                /* posix_memalign block */
        free((void *)(uintptr_t)((uint32_t *)p)[-1]);
        return;
    }
    blk *b = (blk *)((uint8_t *)p - 8);
    b->sz = *(uint32_t *)b & ~1u;
    b->next = g_free;                                        /* LIFO; no coalescing v1 */
    g_free = b;
}

void *calloc(size_t nm, size_t sz)
{
    if (sz && nm > (size_t)-1 / sz) { g_errno = 12; return 0; }
    size_t n = nm * sz;
    uint8_t *p = malloc(n);
    if (p) for (size_t i = 0; i < n; i++) p[i] = 0;
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    uint32_t old = (*(uint32_t *)((uint8_t *)p - 8) & ~1u) - 8u;
    void *q = malloc(n);
    if (!q) return 0;
    size_t cp = old < n ? old : n;
    for (size_t i = 0; i < cp; i++) ((uint8_t *)q)[i] = ((uint8_t *)p)[i];
    free(p);
    return q;
}

int posix_memalign(void **out, size_t align, size_t n)
{
    if (align < 8 || (align & (align - 1))) return 22;       /* EINVAL */
    uint8_t *raw = malloc(n + align + 8);
    if (!raw) return 12;                                      /* ENOMEM */
    uintptr_t a = ((uintptr_t)raw + 8 + align - 1) & ~(uintptr_t)(align - 1);
    ((uint32_t *)a)[-2] = ALIGN_MAGIC;                        /* free() routes on this */
    ((uint32_t *)a)[-1] = (uint32_t)(uintptr_t)raw;           /* the raw malloc ptr    */
    *out = (void *)a;
    return 0;
}

/* ---- fds / io ---- */
int32_t open(const char *path, int32_t flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int32_t mode = va_arg(ap, int32_t);
    va_end(ap);
    return fold(__lvo_open((uint32_t)(uintptr_t)path, (uint32_t)flags, (uint32_t)mode));
}
int32_t close(int32_t fd)               { return fold(__lvo_close((uint32_t)fd)); }
long read(int32_t fd, void *b, size_t n)
{ return (long)fold(__lvo_read((uint32_t)fd, (uint32_t)(uintptr_t)b, (uint32_t)n)); }
long write(int32_t fd, const void *b, size_t n)
{ return (long)fold(__lvo_write((uint32_t)fd, (uint32_t)(uintptr_t)b, (uint32_t)n)); }

int64_t lseek(int32_t fd, int64_t off, int32_t whence)
{
    if (off > 0x7FFFFFFFLL || off < -0x80000000LL) { g_errno = 22; return -1; }
    return (int64_t)fold(__lvo_lseek((uint32_t)fd, (uint32_t)(int32_t)off, (uint32_t)whence));
}

int32_t unlink(const char *p) { return fold(__lvo_delete((uint32_t)(uintptr_t)p)); }
int32_t mkdir(const char *p, uint32_t m) { return fold(__lvo_mkdir((uint32_t)(uintptr_t)p, m)); }
int32_t rmdir(const char *p) { return fold(__lvo_rmdir((uint32_t)(uintptr_t)p)); }
int32_t rename(const char *o, const char *n)
{ (void)o; (void)n; g_errno = 78; return -1; }                /* ENOSYS (v1) */

/* ---- stat: the pal's Attr struct (fs/aros.rs, repr(C)) filled from the 20-byte
 * big-endian stub record, which reads natively here. ---- */
struct attr {
    uint64_t size;
    uint32_t mode;
    uint32_t nlink;
    uint64_t ino;
    int64_t  mtime_sec, mtime_nsec, atime_sec, atime_nsec;
};
struct stubrec { uint32_t kind, size_hi, size_lo, mtime, ro; };

static int32_t attr_from(int32_t r, const struct stubrec *s, struct attr *a)
{
    if (r < 0) { g_errno = -r; return -1; }
    a->size  = ((uint64_t)s->size_hi << 32) | s->size_lo;
    /* mode: S_IFREG 0100000 / S_IFDIR 040000 (NetBSD octal) + rw bits */
    a->mode  = (s->kind == 1 ? 0040000u : s->kind == 0 ? 0100000u : 0020000u)
             | (s->ro ? 0444u : 0644u);
    a->nlink = 1;
    a->ino   = 0;
    a->mtime_sec = (int64_t)s->mtime; a->mtime_nsec = 0;
    a->atime_sec = (int64_t)s->mtime; a->atime_nsec = 0;
    return 0;
}
int32_t aros_stat(const char *p, struct attr *a)
{
    struct stubrec s;
    return attr_from(__lvo_stat((uint32_t)(uintptr_t)p, (uint32_t)(uintptr_t)&s), &s, a);
}
int32_t aros_lstat(const char *p, struct attr *a) { return aros_stat(p, a); }
int32_t aros_fstat(int32_t fd, struct attr *a)
{
    struct stubrec s;
    return attr_from(__lvo_fstat((uint32_t)fd, (uint32_t)(uintptr_t)&s), &s, a);
}

/* dir listing: not in stub-DOS v1 — read_dir reports the error cleanly. */
void *aros_opendir(const char *p) { (void)p; g_errno = 78; return 0; }
int32_t aros_readdir(void *d, char *b, size_t n, uint32_t *t)
{ (void)d; (void)b; (void)n; (void)t; g_errno = 78; return -1; }
void aros_closedir(void *d) { (void)d; }

/* ---- time: the pal's timespec is { i32 tv_sec; i64 tv_nsec } (time/aros.rs);
 * the same C declaration compiles to the identical m68k layout. ---- */
struct ts { int32_t tv_sec; int64_t tv_nsec; };
int32_t clock_gettime(int32_t clk, struct ts *tp)
{
    struct { uint32_t s, ns; } o;
    int32_t r = __lvo_gettime(clk == 2 ? 1u : 0u, (uint32_t)(uintptr_t)&o);
    if (r < 0) { g_errno = -r; return -1; }
    tp->tv_sec = (int32_t)o.s;
    tp->tv_nsec = (int64_t)o.ns;
    return 0;
}

/* ---- entropy / env / args ---- */
void arc4random_buf(void *b, size_t n)
{ __lvo_entropy((uint32_t)(uintptr_t)b, (uint32_t)n); }

/* cwd: the stub-DOS passes paths to the host verbatim; the process cwd is fixed
 * at "/" (run68k's own cwd is the effective base for relative paths). */
char *getcwd(char *buf, size_t size)
{
    if (!buf || size < 2) { g_errno = 22; return 0; }
    buf[0] = '/'; buf[1] = 0;
    return buf;
}
int32_t chdir(const char *p) { (void)p; g_errno = 78; return -1; }

char *getenv(const char *n) { (void)n; return 0; }
int32_t setenv(const char *n, const char *v, int32_t o)
{ (void)n; (void)v; (void)o; g_errno = 78; return -1; }
int32_t unsetenv(const char *n) { (void)n; g_errno = 78; return -1; }

int32_t aros_argc = 1;
static const char *g_argv[2] = { "rust68k", 0 };
const char **aros_argv = g_argv;

/* ---- entry (called by crt0 with the globals parked) ---- */
extern uint32_t rust_main68k(void);
uint32_t __libc68k_start(void)
{
    heap_init();
    return rust_main68k();
}
