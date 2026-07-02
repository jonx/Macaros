/* stublib.c — the minimal STUB AmigaOS library environment (OURS, AROS-licensed).
 *
 * Clean-room / OURS. Implements stublib.h: builds the negative-offset JumpVec table
 * in the [J4] sandbox per arch/m68k-all/include/aros/cpu.h, provides native stub
 * functions for AllocMem/FreeMem/PutChar that RECORD their calls + args, and
 * dispatches a recognised LVO call through the REAL [J3] marshaller
 * (j3_build_marshal_thunk) — the 68k-regs -> AAPCS64 reverse-H3 thunk emitted into a
 * MAP_JIT region. This makes a library-calling 68k program's behaviour observable.
 *
 * The native stubs need the per-call argument values AND a way to touch the harness
 * state (the heap cursor, the call log). The [J3] marshaller calls a plain AAPCS64
 * stub `(uint32_t,...)->uint32_t`, so we thread the active stub_lib through a single
 * file-scope pointer set just before each dispatch (single-threaded harness).
 */
#include "stublib.h"
#include "j3_jit68k.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>    /* stub-DOS: host open/read/write/lseek/unlink/rmdir      */
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

/* ----- big-endian sandbox writers (the JumpVec bytes are 68k big-endian) ------- */
static void sb_w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void sb_w32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

/* The library currently being dispatched (single-threaded harness). The native
 * stubs read/write it. Set by stublib_dispatch before the marshal thunk runs. */
static stub_lib *g_active;
/* The sandbox of the active dispatch: the stub-DOS calls translate 68k pointer
 * args (paths, buffers) into host pointers through it, bounds-checked. */
static j4_sandbox *g_sb;

/* ---- stub-DOS helpers ----------------------------------------------------------
 * A 68k pointer arg is only touched through these: NULL on any out-of-sandbox range. */
static uint8_t *dos_range(uint32_t addr, uint32_t len)
{
    j4_sandbox *sb = g_sb;
    if (!sb || !addr) return NULL;
    if (addr < sb->sandbox_origin) return NULL;
    if ((uint64_t)addr + len > (uint64_t)sb->sandbox_origin + sb->size) return NULL;
    return sb->host_mem + (addr - sb->sandbox_origin);
}
/* A NUL-terminated 68k string (path); bounded scan, NULL if unterminated/OOB. */
static const char *dos_str(uint32_t addr)
{
    uint8_t *p = dos_range(addr, 1);
    if (!p) return NULL;
    uint64_t max = (uint64_t)g_sb->sandbox_origin + g_sb->size - addr;
    for (uint64_t i = 0; i < max && i < 4096; i++)
        if (p[i] == 0) return (const char *)p;
    return NULL;
}
/* Host errno -> the NEGATIVE return convention. darwin and NetBSD (the numbering
 * the Rust pal decodes) agree on the classic values (ENOENT 2, EACCES 13, EEXIST
 * 17, ENOTDIR 20, EISDIR 21, EINVAL 22, ENOSPC 28, EROFS 30, EPIPE 32, EBADF 9);
 * anything exotic degrades to EIO (5). */
static int32_t dos_err(void)
{
    int e = errno;
    if (e <= 0 || e > 90) e = 5;
    return -(int32_t)e;
}
static void dos_record(int lvo, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t ret)
{
    stub_lib *L = g_active;
    if (L->ncalls < STUB_MAX_CALLS) {
        stub_call_rec *r = &L->calls[L->ncalls++];
        r->lvo = lvo; r->arg_d0 = d1; r->arg_d1 = d2; r->arg_a1 = d3; r->ret_d0 = ret;
    }
}
/* The stub-DOS wire flags (STUB_O_*, the AROS posixc values) -> host O_*. */
static int dos_open_flags(uint32_t f)
{
    int h;
    switch (f & 3u) {
        case STUB_O_RDONLY: h = O_RDONLY; break;
        case STUB_O_WRONLY: h = O_WRONLY; break;
        default:            h = O_RDWR;   break;
    }
    if (f & STUB_O_CREAT)  h |= O_CREAT;
    if (f & STUB_O_EXCL)   h |= O_EXCL;
    if (f & STUB_O_TRUNC)  h |= O_TRUNC;
    if (f & STUB_O_APPEND) h |= O_APPEND;
    return h;
}

/* ----- native AAPCS64 stub functions (the bodies the [J3] thunk blr's) ----------
 * Each takes the 68k register args (already marshalled into x0.. by the thunk, per
 * the per-LVO src[] map) and records the call. These are ordinary host functions;
 * the marshaller turns 68k regs into their AAPCS64 args. */

/* AllocMem(d0=byteSize, d1=requirements) -> d0 = 68k sandbox pointer (or 0).
 * AROS_LHA map: (ULONG byteSize, D0), (ULONG requirements, D1). */
static uint32_t stub_AllocMem(uint32_t byteSize, uint32_t requirements)
{
    stub_lib *L = g_active;
    uint32_t aligned = (byteSize + 15u) & ~15u;         /* MEMCHUNK rounding */
    uint32_t ptr = 0;
    if (aligned && L->heap_next + aligned <= L->heap_end) {
        ptr = L->heap_next;
        L->heap_next += aligned;
        L->bytes_outstanding += byteSize;
    }
    /* record */
    if (L->ncalls < STUB_MAX_CALLS) {
        stub_call_rec *r = &L->calls[L->ncalls++];
        r->lvo = STUB_LVO_ALLOCMEM; r->arg_d0 = byteSize; r->arg_d1 = requirements;
        r->arg_a1 = 0; r->ret_d0 = ptr;
    }
    (void)requirements;   /* MEMF_CLEAR honoured implicitly: sandbox is zero-init */
    return ptr;
}

/* FreeMem(a1=memoryBlock, d0=byteSize) -> void.
 * AROS_LHA map: (APTR memoryBlock, A1), (ULONG byteSize, D0). */
static uint32_t stub_FreeMem(uint32_t memoryBlock, uint32_t byteSize)
{
    stub_lib *L = g_active;
    if (byteSize <= L->bytes_outstanding) L->bytes_outstanding -= byteSize;
    if (L->ncalls < STUB_MAX_CALLS) {
        stub_call_rec *r = &L->calls[L->ncalls++];
        r->lvo = STUB_LVO_FREEMEM; r->arg_d0 = byteSize; r->arg_d1 = 0;
        r->arg_a1 = memoryBlock; r->ret_d0 = 0;
    }
    return 0;   /* void LVO; the descriptor sets returns=0 so d0 is untouched */
}

/* PutChar(d0=char) -> void. Observable "print" sink. */
static uint32_t stub_PutChar(uint32_t ch)
{
    stub_lib *L = g_active;
    if (L->outlen < (int)sizeof(L->out) - 1) L->out[L->outlen++] = (char)(ch & 0xff);
    if (L->ncalls < STUB_MAX_CALLS) {
        stub_call_rec *r = &L->calls[L->ncalls++];
        r->lvo = STUB_LVO_PUTCHAR; r->arg_d0 = ch; r->arg_d1 = 0;
        r->arg_a1 = 0; r->ret_d0 = 0;
    }
    return 0;
}

/* ----- the stub-DOS native bodies (args d1,d2,d3 -> return d0) ------------------ */

static int g_dos_log = -1;
static int dos_log(void) { if (g_dos_log < 0) g_dos_log = getenv("JIT68K_DOSLOG") ? 1 : 0; return g_dos_log; }

static uint32_t stub_Open(uint32_t path, uint32_t flags, uint32_t mode)
{
    const char *p = dos_str(path);
    int32_t r;
    if (!p) r = -22 /* EINVAL */;
    else {
        int fd = open(p, dos_open_flags(flags), (mode_t)(mode ? mode : 0666));
        r = (fd < 0) ? dos_err() : fd;
    }
    if (dos_log()) fprintf(stderr, "[dos] Open path=%08x(%s) flags=%08x -> %d\n",
                           path, p ? p : "<oob>", flags, (int)r);
    dos_record(STUB_LVO_OPEN, path, flags, mode, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_Close(uint32_t fd)
{
    int32_t r = 0;
    if ((int32_t)fd >= 3) r = (close((int)fd) < 0) ? dos_err() : 0;
    if (dos_log()) fprintf(stderr, "[dos] Close fd=%d -> %d\n",(int)fd,(int)r);
    dos_record(STUB_LVO_CLOSE, fd, 0, 0, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_Read(uint32_t fd, uint32_t buf, uint32_t len)
{
    int32_t r;
    if (len > 0x7FFFFFFFu) len = 0x7FFFFFFFu;
    uint8_t *h = dos_range(buf, len);
    if (!h && len) r = -22;
    else if (fd == 0) r = 0;                               /* stdin: EOF */
    else if (fd == 1 || fd == 2) r = -9 /* EBADF */;
    else {
        ssize_t n = read((int)fd, h, (size_t)len);
        r = (n < 0) ? dos_err() : (int32_t)n;
    }
    if (dos_log()) fprintf(stderr, "[dos] Read fd=%d buf=%08x len=%u -> %d\n",
                           (int)fd, buf, len, (int)r);
    dos_record(STUB_LVO_READ, fd, buf, len, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_Write(uint32_t fd, uint32_t buf, uint32_t len)
{
    stub_lib *L = g_active;
    int32_t r;
    if (len > 0x7FFFFFFFu) len = 0x7FFFFFFFu;
    uint8_t *h = dos_range(buf, len);
    if (!h && len) r = -22;
    else if (fd == 1 || fd == 2) {                          /* the capture buffer */
        uint32_t room = (uint32_t)sizeof(L->out) - 1u - (uint32_t)L->outlen;
        uint32_t n = len < room ? len : room;
        memcpy(L->out + L->outlen, h, n);
        L->outlen += (int)n;
        r = (int32_t)len;             /* short-write only the harness would see */
    }
    else if (fd == 0) r = -9;
    else {
        ssize_t n = write((int)fd, h, (size_t)len);
        r = (n < 0) ? dos_err() : (int32_t)n;
    }
    if (dos_log()) fprintf(stderr, "[dos] Write fd=%d buf=%08x len=%u -> %d\n",
                                      (int)fd, buf, len, (int)r);
    dos_record(STUB_LVO_WRITE, fd, buf, len, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_LSeek(uint32_t fd, uint32_t off, uint32_t whence)
{
    int32_t r;
    if ((int32_t)fd < 3) r = -29 /* ESPIPE */;
    else {
        off_t n = lseek((int)fd, (off_t)(int32_t)off, (int)whence);
        if (n < 0) r = dos_err();
        else if (n > 0x7FFFFFFF) r = -27 /* EFBIG: >2GiB not representable */;
        else r = (int32_t)n;
    }
    if (dos_log()) fprintf(stderr, "[dos] LSeek fd=%d off=%d whence=%u -> %d\n",(int)fd,(int)off,whence,(int)r);
    dos_record(STUB_LVO_LSEEK, fd, off, whence, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_Delete(uint32_t path)
{
    const char *p = dos_str(path);
    int32_t r = !p ? -22 : (unlink(p) < 0 ? dos_err() : 0);
    dos_record(STUB_LVO_DELETE, path, 0, 0, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_MkDir(uint32_t path, uint32_t mode)
{
    const char *p = dos_str(path);
    int32_t r = !p ? -22 : (mkdir(p, (mode_t)(mode ? mode : 0777)) < 0 ? dos_err() : 0);
    dos_record(STUB_LVO_MKDIR, path, mode, 0, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_RmDir(uint32_t path)
{
    const char *p = dos_str(path);
    int32_t r = !p ? -22 : (rmdir(p) < 0 ? dos_err() : 0);
    dos_record(STUB_LVO_RMDIR, path, 0, 0, (uint32_t)r);
    return (uint32_t)r;
}

/* The 20-byte big-endian stat record (stublib.h): kind, size_hi, size_lo,
 * mtime_secs, readonly. */
static void dos_stat_rec(uint8_t *o, const struct stat *st)
{
    uint32_t kind = S_ISREG(st->st_mode) ? 0u : S_ISDIR(st->st_mode) ? 1u : 2u;
    uint64_t sz = (uint64_t)st->st_size;
    sb_w32(o + 0, kind);
    sb_w32(o + 4, (uint32_t)(sz >> 32));
    sb_w32(o + 8, (uint32_t)sz);
    sb_w32(o + 12, (uint32_t)st->st_mtime);
    sb_w32(o + 16, (st->st_mode & 0200) ? 0u : 1u);
}

static uint32_t stub_Stat(uint32_t path, uint32_t rec)
{
    const char *p = dos_str(path);
    uint8_t *o = dos_range(rec, 20);
    int32_t r;
    struct stat st;
    if (!p || !o) r = -22;
    else if (stat(p, &st) < 0) r = dos_err();
    else { dos_stat_rec(o, &st); r = 0; }
    if (dos_log()) fprintf(stderr, "[dos] Stat path=%08x(%s) rec=%08x -> %d\n",
                           path, p ? p : "<oob>", rec, (int)r);
    dos_record(STUB_LVO_STAT, path, rec, 0, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_FStat(uint32_t fd, uint32_t rec)
{
    uint8_t *o = dos_range(rec, 20);
    int32_t r;
    struct stat st;
    if (!o) r = -22;
    else if (fstat((int)fd, &st) < 0) r = dos_err();
    else { dos_stat_rec(o, &st); r = 0; }
    dos_record(STUB_LVO_FSTAT, fd, rec, 0, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_GetTime(uint32_t which, uint32_t outp)
{
    uint8_t *o = dos_range(outp, 8);
    int32_t r;
    struct timespec ts;
    if (!o) r = -22;
    else if (clock_gettime(which == 1 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts) < 0)
        r = dos_err();
    else {
        sb_w32(o + 0, (uint32_t)ts.tv_sec);
        sb_w32(o + 4, (uint32_t)ts.tv_nsec);
        r = 0;
    }
    dos_record(STUB_LVO_GETTIME, which, outp, 0, (uint32_t)r);
    return (uint32_t)r;
}

static uint32_t stub_Entropy(uint32_t buf, uint32_t len)
{
    uint8_t *h = dos_range(buf, len);
    int32_t r;
    if (!h && len) r = -22;
    else { arc4random_buf(h, (size_t)len); r = 0; }
    dos_record(STUB_LVO_ENTROPY, buf, len, 0, (uint32_t)r);
    return (uint32_t)r;
}

/* ----- the LVO -> (native stub, register map) table ----------------------------
 * Each entry is a [J3] descriptor: the ORDERED source 68k registers (the
 * AROS_LHA/AROS_UFHA `reg` list) feeding the native stub, and whether the return
 * goes back into d0. Grounded in j3_jit68k.h note B (the reg list IS the map). */
static const j3_lvo_desc *lvo_desc(int lvo)
{
    /* Built lazily; the libbase field is filled in by stublib_dispatch (it varies
     * per library instance). nargs/src/returns are the fixed AROS_LH maps. */
    static j3_lvo_desc allocmem = {
        "AllocMem", 0, STUB_LVO_ALLOCMEM, 2, { J3_D0, J3_D1 }, 1, (void(*)(void))stub_AllocMem
    };
    static j3_lvo_desc freemem = {
        "FreeMem", 0, STUB_LVO_FREEMEM, 2, { J3_A1, J3_D0 }, 0, (void(*)(void))stub_FreeMem
    };
    static j3_lvo_desc putchar_ = {
        "PutChar", 0, STUB_LVO_PUTCHAR, 1, { J3_D0 }, 0, (void(*)(void))stub_PutChar
    };
    /* the stub-DOS set: args d1/d2/d3, return in d0 (returns=1 for every call —
     * even the 0-or-negative-errno ones return their status through d0). */
    static j3_lvo_desc dopen = {
        "Open", 0, STUB_LVO_OPEN, 3, { J3_D1, J3_D2, J3_D3 }, 1, (void(*)(void))stub_Open
    };
    static j3_lvo_desc dclose = {
        "Close", 0, STUB_LVO_CLOSE, 1, { J3_D1 }, 1, (void(*)(void))stub_Close
    };
    static j3_lvo_desc dread = {
        "Read", 0, STUB_LVO_READ, 3, { J3_D1, J3_D2, J3_D3 }, 1, (void(*)(void))stub_Read
    };
    static j3_lvo_desc dwrite = {
        "Write", 0, STUB_LVO_WRITE, 3, { J3_D1, J3_D2, J3_D3 }, 1, (void(*)(void))stub_Write
    };
    static j3_lvo_desc dlseek = {
        "LSeek", 0, STUB_LVO_LSEEK, 3, { J3_D1, J3_D2, J3_D3 }, 1, (void(*)(void))stub_LSeek
    };
    static j3_lvo_desc ddelete = {
        "Delete", 0, STUB_LVO_DELETE, 1, { J3_D1 }, 1, (void(*)(void))stub_Delete
    };
    static j3_lvo_desc dmkdir = {
        "MkDir", 0, STUB_LVO_MKDIR, 2, { J3_D1, J3_D2 }, 1, (void(*)(void))stub_MkDir
    };
    static j3_lvo_desc drmdir = {
        "RmDir", 0, STUB_LVO_RMDIR, 1, { J3_D1 }, 1, (void(*)(void))stub_RmDir
    };
    static j3_lvo_desc dstat = {
        "Stat", 0, STUB_LVO_STAT, 2, { J3_D1, J3_D2 }, 1, (void(*)(void))stub_Stat
    };
    static j3_lvo_desc dfstat = {
        "FStat", 0, STUB_LVO_FSTAT, 2, { J3_D1, J3_D2 }, 1, (void(*)(void))stub_FStat
    };
    static j3_lvo_desc dgettime = {
        "GetTime", 0, STUB_LVO_GETTIME, 2, { J3_D1, J3_D2 }, 1, (void(*)(void))stub_GetTime
    };
    static j3_lvo_desc dentropy = {
        "Entropy", 0, STUB_LVO_ENTROPY, 2, { J3_D1, J3_D2 }, 1, (void(*)(void))stub_Entropy
    };
    switch (lvo) {
        case STUB_LVO_ALLOCMEM: return &allocmem;
        case STUB_LVO_FREEMEM:  return &freemem;
        case STUB_LVO_PUTCHAR:  return &putchar_;
        case STUB_LVO_OPEN:     return &dopen;
        case STUB_LVO_CLOSE:    return &dclose;
        case STUB_LVO_READ:     return &dread;
        case STUB_LVO_WRITE:    return &dwrite;
        case STUB_LVO_LSEEK:    return &dlseek;
        case STUB_LVO_DELETE:   return &ddelete;
        case STUB_LVO_MKDIR:    return &dmkdir;
        case STUB_LVO_RMDIR:    return &drmdir;
        case STUB_LVO_STAT:     return &dstat;
        case STUB_LVO_FSTAT:    return &dfstat;
        case STUB_LVO_GETTIME:  return &dgettime;
        case STUB_LVO_ENTROPY:  return &dentropy;
        default:                return NULL;
    }
}

/* ----- the JumpVec table builder ----------------------------------------------- */
int stublib_init(stub_lib *lib, j4_sandbox *sb, uint32_t libbase,
                 uint32_t heap_base, uint32_t heap_end)
{
    memset(lib, 0, sizeof(*lib));
    lib->libbase   = libbase;
    lib->heap_base = heap_base;
    lib->heap_end  = heap_end;
    lib->heap_next = heap_base;

    /* Lay down a FULLJMP vector (0x4EF9 <sentinel32>) for each implemented LVO at
     * byte address libbase - n*6 (cpu.h __AROS_GETJUMPVEC, LIB_VECTSIZE==6). The
     * <sentinel> target is never executed by the harness; the dispatcher recognises
     * the jsr-through-vector PC and routes to the native stub. We still write the
     * real 0x4EF9 so the table is byte-faithful to what MakeLibrary would build. */
    const int lvos[] = { STUB_LVO_PUTCHAR, STUB_LVO_ALLOCMEM, STUB_LVO_FREEMEM,
                         STUB_LVO_OPEN, STUB_LVO_CLOSE, STUB_LVO_READ, STUB_LVO_WRITE,
                         STUB_LVO_LSEEK, STUB_LVO_DELETE, STUB_LVO_MKDIR, STUB_LVO_RMDIR,
                         STUB_LVO_STAT, STUB_LVO_FSTAT, STUB_LVO_GETTIME, STUB_LVO_ENTROPY };
    for (unsigned i = 0; i < sizeof(lvos)/sizeof(lvos[0]); i++) {
        int n = lvos[i];
        uint32_t vaddr = libbase - (uint32_t)n * J3_M68K_LIB_VECTSIZE;   /* lib - n*6 */
        /* bounds: the vector (6 bytes) must lie inside the sandbox */
        if (vaddr < sb->sandbox_origin ||
            (uint64_t)vaddr + 6u > (uint64_t)sb->sandbox_origin + sb->size)
            return 1;
        uint8_t *p = j4_sandbox_host(sb, vaddr);
        sb_w16(p, 0x4EF9u);                              /* __AROS_ASMJMP */
        sb_w32(p + 2, 0xC0ED0000u | (uint32_t)n);        /* sentinel (cpu.h _aros_empty_vector style) */
    }
    return 0;
}

uint32_t stublib_vector_addr(const stub_lib *lib, int lvo)
{
    return lib->libbase - (uint32_t)lvo * J3_M68K_LIB_VECTSIZE;
}

/* Per-LVO marshal-thunk cache. The [J3] marshaller emits one MAP_JIT region per build and
 * holds a small pool (J3_MAX_THUNKS). A library-calling program in a LOOP (e.g. the [J5j]
 * Mandelbrot, which calls PutChar ~1700 times) would exhaust that pool if a fresh thunk were
 * built per call. The thunk for a given LVO is FIXED (its source-register map never changes),
 * so build it ONCE and reuse it — exactly what a real library base does (the JumpVec is set
 * up once at MakeLibrary time). Keyed by the LVO index; the three stub LVOs need three thunks
 * total, well within the pool. (Single-threaded harness; no locking needed.) */
typedef struct { int lvo; j3_thunk_fn thunk; } thunk_cache_ent;
static thunk_cache_ent g_thunk_cache[24];   /* the full stub-DOS LVO set fits */
static int             g_thunk_cache_n = 0;

static j3_thunk_fn cached_thunk(const j3_lvo_desc *desc, char *errbuf, unsigned errlen)
{
    for (int i = 0; i < g_thunk_cache_n; i++)
        if (g_thunk_cache[i].lvo == desc->lvo) return g_thunk_cache[i].thunk;
    j3_thunk_fn t = j3_build_marshal_thunk(desc, errbuf, errlen);
    if (!t) return NULL;
    if (g_thunk_cache_n < (int)(sizeof g_thunk_cache / sizeof g_thunk_cache[0])) {
        g_thunk_cache[g_thunk_cache_n].lvo = desc->lvo;
        g_thunk_cache[g_thunk_cache_n].thunk = t;
        g_thunk_cache_n++;
    }
    return t;
}

/* Drop the thunk cache (the underlying regions are freed by j3_free_all_thunks). Call this
 * after each program run so a fresh run rebuilds against a fresh [J3] pool. */
void stublib_reset_thunk_cache(void) { g_thunk_cache_n = 0; }

int stublib_dispatch(stub_lib *lib, j4_sandbox *sb, int lvo,
                     struct M68KState *st, char *errbuf, unsigned errlen)
{
    if (dos_log()) fprintf(stderr, "[dos] dispatch lvo=%d\n", lvo);
    const j3_lvo_desc *base = lvo_desc(lvo);
    if (!base) { if (errbuf) snprintf(errbuf, errlen, "LVO %d not implemented", lvo); return 1; }

    /* Per-dispatch descriptor copy with this library's base filled in. */
    j3_lvo_desc desc = *base;
    desc.libbase = lib->libbase;

    /* Build (once, then cache) the reverse-H3 marshal thunk via the REAL [J3] marshaller
     * (emitted into a MAP_JIT region), then invoke it: it reads the source 68k registers from
     * the M68KState, places them in AAPCS64 x0..x7, blr's the native stub, and (if the LVO
     * returns) stores the native return into st->d[0]. */
    g_active = lib;
    g_sb = sb;
    j3_thunk_fn thunk = cached_thunk(&desc, errbuf, errlen);
    if (!thunk) { g_active = NULL; g_sb = NULL; return 1; }
    thunk(st);
    g_active = NULL;
    g_sb = NULL;
    return 0;
}
