/* aros_fs_glue.c -- stat/lstat/fstat for the Rust fs pal (sys/fs/aros.rs metadata).
 *
 * Unlike the net/thread glues, this one CAN include the real posixc headers: with
 * -I <tree>/gen/include/aros/posixc, <sys/stat.h> resolves to AROS's own
 * aros/posixc/sys/stat.h (pulling only the aros/types headers), not the macOS SDK. So C
 * own `struct stat` (whose exact layout is preprocessor-conditional and risky to lay
 * out by hand in Rust) and hand back a flat, fixed-layout `aros_fileattr`.
 *
 * Independent work: from the AROS posixc headers only.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>   /* rename() -- implicit decls are an error on clang 16+ */
#include <proto/dos.h>   /* IoErr() */
#include <libraries/stdc.h>  /* struct StdCBase + __aros_getbase_StdCBase() */

/* The errno cell in the task's shared StdCBase.
 *
 * There are two errno cells, an accident of linkage: __stdc_geterrnoptr() is a
 * link-library function, so each image carries its own copy and its own hook
 * variable. pthread installs the per-thread hook in the application's copy, so
 * in-app code gets a per-thread cell -- while stdc.library and posixc.library,
 * being their own images with a NULL hook, write this base cell. A reader that
 * only looks at the per-thread cell therefore misses every error a library
 * set; std's errno() consults this when the per-thread cell says nothing. */
int aros_libbase_errno(void)
{
    struct StdCBase *base = __aros_getbase_StdCBase();
    return base ? base->_errno : 0;
}

/* Clear both cells before a call whose failure we intend to attribute; the
 * shared one holds whatever error some earlier library call left there. */
static void errno_clear(void)
{
    struct StdCBase *base = __aros_getbase_StdCBase();
    errno = 0;
    if (base)
        base->_errno = 0;
}

/* stdc.library's own IoErr()->errno mapping, the one its mkdir and readlink
 * already use. A hand-kept subset sat here briefly and manufactured a wrong
 * answer: it defaulted unknown codes to ENOENT, and the first unknown code to
 * arrive was ERROR_OBJECT_EXISTS -- turning "already exists" into "not found"
 * for the one caller (the editor's case-sensitivity check) whose whole
 * question was "does this already exist". The canonical table knows EEXIST. */
extern int __stdc_ioerr2errno(int ioerr);

/* Recover a reason from IoErr() for a call that failed leaving errno unset.
 * `fallback` is the errno to use when dos will not say either. */
static int errno_from_ioerr(int fallback)
{
    if (errno == 0) {
        /* The library's own report first: posixc and stdc write the shared
         * base cell (see aros_libbase_errno above), and an error they set
         * without a failing dos call -- EEXIST from an O_EXCL check -- exists
         * nowhere else. Guessing from IoErr() before looking there turned
         * that EEXIST into a stale-IoErr ENOENT. */
        int e = aros_libbase_errno();
        if (!e)
            e = __stdc_ioerr2errno(IoErr());
        errno = e ? e : fallback;
    }
    return -1;
}

/* open(), which fails the same way: the editor's attempt to work out whether
 * the filesystem is case sensitive creates a file in a temp directory, and a
 * failure there came back with errno 0 and so no reason at all. */
int aros_open(const char *path, int flags, unsigned int mode)
{
    int fd;
    if (!path) { errno = EINVAL; return -1; }
    errno_clear();
    fd = open(path, flags, mode);
    if (fd < 0)
        return errno_from_ioerr(ENOENT);
    return fd;
}

/* The path-taking calls, wrapped for the same reason as open: clear both
 * errno cells going in, recover a reason coming out. Without the clear, a
 * failure here reads back whatever stale value the shared cell held -- which
 * is exactly what happened to mkdir once errno() learned to consult that
 * cell: create_dir_all on an existing directory read a stale ENOENT instead
 * of its EEXIST, and concluded the parent was missing. */
int aros_mkdir(const char *path, unsigned int mode)
{
    if (!path) { errno = EINVAL; return -1; }
    errno_clear();
    if (mkdir(path, (mode_t)mode) != 0)
        return errno_from_ioerr(ENOENT);
    return 0;
}

int aros_rmdir(const char *path)
{
    if (!path) { errno = EINVAL; return -1; }
    errno_clear();
    if (rmdir(path) != 0)
        return errno_from_ioerr(ENOENT);
    return 0;
}

int aros_unlink(const char *path)
{
    if (!path) { errno = EINVAL; return -1; }
    errno_clear();
    if (unlink(path) != 0)
        return errno_from_ioerr(ENOENT);
    return 0;
}

int aros_rename(const char *from, const char *to)
{
    if (!from || !to) { errno = EINVAL; return -1; }
    errno_clear();
    if (rename(from, to) != 0)
        return errno_from_ioerr(ENOENT);
    return 0;
}

/* A stat that failed without saying why.
 *
 * AROS reports failures through IoErr(), and that does not reliably reach
 * errno: asking about something that is not there returns -1 with errno still
 * 0. Rust reads errno 0 as `Uncategorized`, so it cannot tell "gone" from
 * "broken" -- and a directory scan racing a file that is being removed turns
 * on exactly that. Read as a hard error it abandons the scan; read as "gone"
 * it steps over the entry and carries on. */
static int stat_failed(void)
{
    /* Everything else a stat fails with means the object could not be reached,
       which to a caller is the same as not being there. */
    return errno_from_ioerr(ENOENT);
}

/* Fixed layout the Rust pal mirrors 1:1 (all naturally aligned, 8-byte alignment). */
struct aros_fileattr {
    unsigned long long size;
    unsigned int       mode;
    unsigned int       nlink;
    unsigned long long ino;
    long long          mtime_sec; long long mtime_nsec;
    long long          atime_sec; long long atime_nsec;
    long long          ctime_sec; long long ctime_nsec;
};

static void fill(struct aros_fileattr *o, const struct stat *sb)
{
    o->size      = (unsigned long long)sb->st_size;
    o->mode      = (unsigned int)sb->st_mode;
    o->nlink     = (unsigned int)sb->st_nlink;
    o->ino       = (unsigned long long)sb->st_ino;
    o->mtime_sec = (long long)sb->st_mtim.tv_sec; o->mtime_nsec = (long long)sb->st_mtim.tv_nsec;
    o->atime_sec = (long long)sb->st_atim.tv_sec; o->atime_nsec = (long long)sb->st_atim.tv_nsec;
    o->ctime_sec = (long long)sb->st_ctim.tv_sec; o->ctime_nsec = (long long)sb->st_ctim.tv_nsec;
}

int aros_stat(const char *path, struct aros_fileattr *out)
{
    struct stat sb;
    if (!path || !out) { errno = EINVAL; return -1; }
    /* Cleared first: a stat that fails without touching errno would otherwise
       be reported as whatever the last unrelated call left there. */
    errno_clear();
    if (stat(path, &sb) != 0) return stat_failed();
    fill(out, &sb);
    return 0;
}

int aros_lstat(const char *path, struct aros_fileattr *out)
{
    struct stat sb;
    if (!path || !out) { errno = EINVAL; return -1; }
    /* Cleared first: a stat that fails without touching errno would otherwise
       be reported as whatever the last unrelated call left there. */
    errno_clear();
    if (lstat(path, &sb) != 0) return stat_failed();
    fill(out, &sb);
    return 0;
}

int aros_fstat(int fd, struct aros_fileattr *out)
{
    struct stat sb;
    if (!out) { errno = EINVAL; return -1; }
    /* Cleared first: a stat that fails without touching errno would otherwise
       be reported as whatever the last unrelated call left there. */
    errno_clear();
    if (fstat(fd, &sb) != 0) return stat_failed();
    fill(out, &sb);
    return 0;
}

/* --- directory listing (opendir/readdir/closedir) --------------------------- */

void *aros_opendir(const char *path)
{
    void *d;
    if (!path) { errno = EINVAL; return (void *)0; }
    errno_clear();
    d = (void *)opendir(path);
    if (!d)
        errno_from_ioerr(ENOENT);
    return d;
}

/* Reads the next entry, skipping "." and "..". Copies the name into namebuf and the
 * d_type into *type_out. Returns 1 (entry), 0 (end of directory), -1 (bad args). */
int aros_readdir(void *dir, char *namebuf, unsigned long buflen, unsigned int *type_out)
{
    struct dirent *de;
    unsigned long i;

    if (!dir || !namebuf || buflen == 0)
        return -1;

    for (;;) {
        de = readdir((DIR *)dir);
        if (!de)
            return 0;                         /* end of directory */
        /* skip "." and ".." to match std::fs::read_dir */
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        for (i = 0; de->d_name[i] && i < buflen - 1; i++)
            namebuf[i] = de->d_name[i];
        namebuf[i] = '\0';
        if (type_out)
            *type_out = (unsigned int)de->d_type;
        return 1;
    }
}

void aros_closedir(void *dir)
{
    if (dir)
        closedir((DIR *)dir);
}

/* --- canonicalize (realpath) ------------------------------------------------ *
 * posixc realpath() cannot work in AROS-native path mode: its first act is
 * open(".", O_RDONLY) to save the cwd, and "." is ERROR_INVALID_COMPONENT_NAME
 * to DOS (EINVAL), so every call fails before looking at the input (proved by
 * the RS3g probe, 2026-07-18). Canonicalize the DOS way instead: Lock() the
 * path and ask NameFromLock() for the absolute volume-rooted name. No cwd
 * games, no "." components, symlinks resolved by the handler itself. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/nodes.h>
#include <errno.h>

int aros_realpath(const char *path, char *buf, unsigned long buflen)
{
    struct Process *self;
    APTR oldwindow = NULL;
    int quieted = 0;
    BPTR lock;
    LONG ok;
    if (!path || !buf || buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    /* A path this cannot resolve must come back as an error. Without this,
     * naming an unmounted volume puts up a modal "please insert volume"
     * requester and the caller waits on a click that no one is there to make. */
    self = (struct Process *)FindTask(NULL);
    if (self && self->pr_Task.tc_Node.ln_Type == NT_PROCESS) {
        oldwindow = self->pr_WindowPtr;
        self->pr_WindowPtr = (APTR)-1;
        quieted = 1;
    }
    lock = Lock((CONST_STRPTR)path, SHARED_LOCK);
    if (quieted)
        self->pr_WindowPtr = oldwindow;
    if (!lock) {
        LONG e = IoErr();
        errno = (e == ERROR_LINE_TOO_LONG)              ? ENAMETOOLONG
              : (e == ERROR_INVALID_COMPONENT_NAME
                 || e == ERROR_OBJECT_WRONG_TYPE)       ? EINVAL
              : (e == ERROR_WRITE_PROTECTED
                 || e == ERROR_READ_PROTECTED)          ? EACCES
              : (e == ERROR_OBJECT_IN_USE)              ? EBUSY
                                                        : ENOENT;
        return -1;
    }
    ok = NameFromLock(lock, (STRPTR)buf, (LONG)buflen);
    UnLock(lock);
    if (!ok) {
        errno = (IoErr() == ERROR_LINE_TOO_LONG) ? ENAMETOOLONG : EIO;
        return -1;
    }
    return 0;
}

/* --- file times (set_times) ------------------------------------------------- *
 * posixc has utimes() (path + struct timeval[2]) but no futimes/lutimes/utimensat,
 * so the fd-based File::set_times and the nofollow variant stay Unsupported in the
 * pal. We take sec/nsec pairs and build the timeval[2] on the C side so the Rust pal
 * never lays out struct timeval (usec, not nsec). times[0]=atime, times[1]=mtime. */
int aros_utimes(const char *path,
                long long atime_sec, long long atime_nsec,
                long long mtime_sec, long long mtime_nsec)
{
    struct timeval tv[2];
    if (!path)
        return -1;
    tv[0].tv_sec  = (long)atime_sec;
    tv[0].tv_usec = (long)(atime_nsec / 1000);
    tv[1].tv_sec  = (long)mtime_sec;
    tv[1].tv_usec = (long)(mtime_nsec / 1000);
    return utimes(path, tv);
}
