/* test_libc.c — libc68k smoke test, run BEFORE Rust is in the picture so a
 * failure here is a libc68k/stub-DOS bug, not a Rust one. Exports rust_main68k
 * (the entry libc68k's crt0 calls) and exercises: write(1), malloc/free/
 * posix_memalign, open/write/read/lseek/close/unlink, stat, clock_gettime,
 * arc4random_buf. Prints one line per subsystem; exit 0 only if ALL pass. */
#include <stddef.h>
#include <stdint.h>

extern long write(int32_t fd, const void *b, size_t n);
extern int32_t open(const char *p, int32_t flags, ...);
extern int32_t close(int32_t fd);
extern long read(int32_t fd, void *b, size_t n);
extern int64_t lseek(int32_t fd, int64_t off, int32_t whence);
extern int32_t unlink(const char *p);
extern void *malloc(size_t n);
extern void free(void *p);
extern int posix_memalign(void **out, size_t align, size_t n);
struct attr {
    uint64_t size; uint32_t mode; uint32_t nlink; uint64_t ino;
    int64_t mtime_sec, mtime_nsec, atime_sec, atime_nsec;
};
extern int32_t aros_stat(const char *p, struct attr *a);
struct ts { int32_t tv_sec; int64_t tv_nsec; };
extern int32_t clock_gettime(int32_t clk, struct ts *tp);
extern void arc4random_buf(void *b, size_t n);
extern int32_t *__stdc_geterrnoptr(void);

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { write(1, s, slen(s)); }
static void putnum(uint32_t v)
{
    char b[12]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) { char c = b[--i]; write(1, &c, 1); }
}
static int fails;
static void check(const char *what, int ok)
{
    puts1("[libc68k] "); puts1(what); puts1(ok ? ": ok\n" : ": FAIL\n");
    if (!ok) fails++;
}

uint32_t rust_main68k(void)
{
    /* heap */
    char *a = malloc(1000), *b = malloc(50);
    int heap_ok = a && b && (((uintptr_t)a & 7) == 0);
    if (heap_ok) { for (int i = 0; i < 1000; i++) a[i] = (char)i; free(a); free(b); }
    void *al = 0;
    heap_ok = heap_ok && posix_memalign(&al, 64, 100) == 0 && (((uintptr_t)al & 63) == 0);
    if (al) { ((char *)al)[99] = 7; free(al); }
    check("malloc/memalign", heap_ok);

    /* file round-trip */
    const char *path = "/tmp/libc68k-test.txt";
    int32_t fd = open(path, 0x0002 | 0x0040 | 0x0200, 0666);   /* WRONLY|CREAT|TRUNC */
    int fs_ok = fd >= 3;
    if (fs_ok) fs_ok = write(fd, "stub-dos file io", 16) == 16 && close(fd) == 0;
    if (fs_ok) {
        fd = open(path, 0x0001, 0);                             /* RDONLY */
        char buf[32];
        fs_ok = fd >= 3 && lseek(fd, 9, 0) == 9 && read(fd, buf, 7) == 7
                && buf[0] == 'f' && buf[6] == 'o' && close(fd) == 0;
    }
    check("open/write/lseek/read", fs_ok);

    /* stat + errno */
    struct attr at;
    int st_ok = aros_stat(path, &at) == 0 && at.size == 16 && (at.mode & 0100000u) != 0;
    check("stat", st_ok);
    int rm_ok = unlink(path) == 0 && aros_stat(path, &at) == -1
                && *__stdc_geterrnoptr() == 2;                  /* ENOENT */
    check("unlink+ENOENT", rm_ok);

    /* time: monotonic must not go backwards across two reads */
    struct ts t1, t2;
    int tm_ok = clock_gettime(0, &t1) == 0 && clock_gettime(0, &t2) == 0
                && (t2.tv_sec > t1.tv_sec
                    || (t2.tv_sec == t1.tv_sec && t2.tv_nsec >= t1.tv_nsec));
    int rt_ok = clock_gettime(2, &t1) == 0 && t1.tv_sec > 1700000000;   /* after 2023 */
    check("clock monotonic", tm_ok);
    check("clock realtime", rt_ok);

    /* entropy: two 8-byte draws should differ */
    uint8_t r1[8], r2[8];
    arc4random_buf(r1, 8); arc4random_buf(r2, 8);
    int same = 1;
    for (int i = 0; i < 8; i++) if (r1[i] != r2[i]) same = 0;
    check("entropy", !same);

    puts1("[libc68k] fails="); putnum((uint32_t)fails); puts1("\n");
    return (uint32_t)fails;
}
