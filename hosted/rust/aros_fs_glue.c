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
    if (!path || !out || stat(path, &sb) != 0) return -1;
    fill(out, &sb);
    return 0;
}

int aros_lstat(const char *path, struct aros_fileattr *out)
{
    struct stat sb;
    if (!path || !out || lstat(path, &sb) != 0) return -1;
    fill(out, &sb);
    return 0;
}

int aros_fstat(int fd, struct aros_fileattr *out)
{
    struct stat sb;
    if (!out || fstat(fd, &sb) != 0) return -1;
    fill(out, &sb);
    return 0;
}
