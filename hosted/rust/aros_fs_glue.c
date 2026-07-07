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

/* --- directory listing (opendir/readdir/closedir) --------------------------- */

void *aros_opendir(const char *path)
{
    return path ? (void *)opendir(path) : (void *)0;
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
