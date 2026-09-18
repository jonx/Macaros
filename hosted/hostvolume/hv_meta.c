/* hv_meta.c — the .ameta per-directory metadata file (R-SIDECAR), host side.
 *
 * Implemented from aros-next docs/features/file-metadata/ameta.md, the format
 * Macaros host volumes, AFS+ tools and Pkg share. ameta.c parses and writes
 * the format; this file finds the directory's .ameta, reads it, and replaces
 * it atomically. Independent work: no third-party implementation source was
 * read or consulted.
 *
 * One .ameta per directory holds the metadata of that directory's entries.
 * The comment travels as UTF-8 and reaches AROS as Latin-1, through the same
 * reversible escape names use. A write re-reads the file, applies the change,
 * drops entries whose file is gone, writes a temporary file beside it, checks
 * that nobody replaced .ameta meanwhile (and starts again if someone did),
 * then renames it into place, or deletes .ameta when nothing is left.
 */
#include "hostvolume.h"
#include "ameta.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define META_NAME "/.ameta"

#ifdef __APPLE__
#define MTIME_NS(st) ((long long)(st).st_mtimespec.tv_sec * 1000000000LL + (st).st_mtimespec.tv_nsec)
#else
#define MTIME_NS(st) ((long long)(st).st_mtim.tv_sec * 1000000000LL + (st).st_mtim.tv_nsec)
#endif
#define WRITE_ATTEMPTS 8
#define PATH_CAP 4096

/* ---- paths ----------------------------------------------------------------- */

/* Split "dir/name" into the directory (without its slash) and the name. */
static int split(const char *path, char *dir, size_t dircap, const char **name)
{
    const char *slash = strrchr(path, '/');
    size_t dlen = slash ? (size_t)(slash - path) : 1;
    if (dlen + 1 > dircap)
        return -1;
    if (slash)
        memcpy(dir, path, dlen);
    else
        dir[0] = '.';
    dir[dlen] = '\0';
    *name = slash ? slash + 1 : path;
    return 0;
}

int hv_is_sidecar_name(const char *name)
{
    /* .ameta itself, and a writer's temporary file ".ameta.XXXXXX". */
    return strcmp(name, ".ameta") == 0 || strncmp(name, ".ameta.", 7) == 0;
}

int hv_meta_is_default(const HVMeta *m)
{
    return (m->prot & ~(uint32_t)AMETA_RECORD_MASK) == 0 && m->comment[0] == '\0';
}

/* ---- reading --------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len;
    int exists;
    struct stat id;
    ameta_arena arena;
    ameta_file file;
    int usable;
} loaded;

static void report(void *ctx, unsigned line, const char *why)
{
    fprintf(stderr, "hostvolume: %s line %u: %s\n", (const char *)ctx, line, why);
}

static void unload(loaded *l)
{
    free(l->buf);
    free(l->arena.base);
    memset(l, 0, sizeof *l);
}

/* Read dir/.ameta. -1 on an I/O error; otherwise l->usable says whether the
 * file may be rewritten (a file of another version never is). */
static int load(const char *dir, loaded *l)
{
    char path[4096];
    int fd;
    memset(l, 0, sizeof *l);
    snprintf(path, sizeof path, "%s%s", dir, META_NAME);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT)
            return -1;
    } else {
        ssize_t r;
        size_t cap = 4096;
        if (fstat(fd, &l->id) != 0) {
            close(fd);
            return -1;
        }
        l->exists = 1;
        l->buf = malloc(cap);
        while (l->buf && (r = read(fd, l->buf + l->len, cap - l->len)) > 0) {
            l->len += (size_t)r;
            if (l->len == cap) {
                if (cap > AMETA_MAX_FILE)
                    break;
                cap *= 2;
                l->buf = realloc(l->buf, cap);
            }
        }
        close(fd);
        if (!l->buf)
            return -1;
    }
    l->arena.cap = l->len * 4 + 65536;
    l->arena.base = malloc(l->arena.cap);
    if (!l->arena.base)
        return -1;
    l->usable = ameta_parse(l->buf ? l->buf : "", l->len, &l->arena, &l->file, report, path)
                == AMETA_OK;
    return 0;
}

int hv_meta_read(const char *filepath, HVMeta *out)
{
    char dir[4096];
    const char *name;
    loaded l;
    ameta_entry *e;
    int found = 0;

    out->prot = HV_PROT_DEFAULT;
    out->comment[0] = '\0';
    if (split(filepath, dir, sizeof dir, &name) != 0 || load(dir, &l) != 0)
        return -1;
    e = l.usable ? ameta_find(&l.file, (const unsigned char *)name, strlen(name)) : NULL;
    if (e) {
        if (e->has_prot)
            out->prot = (uint32_t)e->prot;
        ameta_comment_to_latin1(e->comment, e->comment_len, out->comment, sizeof out->comment);
        found = 1;
    }
    unload(&l);
    return found;
}

/* ---- writing --------------------------------------------------------------- */

typedef struct {
    const char *dir;
} directory;

static int present(void *ctx, const unsigned char *name, size_t len)
{
    char path[4096];
    struct stat st;
    const directory *d = ctx;
    snprintf(path, sizeof path, "%s/%.*s", d->dir, (int)len, (const char *)name);
    return lstat(path, &st) == 0;
}

static int same_identity(const loaded *l, const char *path)
{
    struct stat now;
    if (stat(path, &now) != 0)
        return !l->exists && errno == ENOENT;
    return l->exists && now.st_dev == l->id.st_dev && now.st_ino == l->id.st_ino
           && now.st_size == l->id.st_size
           && MTIME_NS(now) == MTIME_NS(l->id);
}

typedef int (*change_fn)(ameta_file *f, ameta_arena *a, void *arg);

/* Read, change, prune and replace dir/.ameta, starting again when another
 * writer replaced it meanwhile. */
static int rewrite_locked(const char *dir, char *path, char *tmp, directory *dp,
                          change_fn change, void *arg);

static int rewrite(const char *dir, change_fn change, void *arg)
{
    char path[PATH_CAP], tmp[PATH_CAP];
    directory d = { dir };
    int lock, rc = -1;
    snprintf(path, sizeof path, "%s%s", dir, META_NAME);
    /* Writers of one directory's .ameta take turns on the directory itself;
     * the identity check below guards against a writer that cannot lock. */
    lock = open(dir, O_RDONLY);
    if (lock >= 0)
        flock(lock, LOCK_EX);
    rc = rewrite_locked(dir, path, tmp, &d, change, arg);
    if (lock >= 0) {
        flock(lock, LOCK_UN);
        close(lock);
    }
    return rc;
}

static int rewrite_locked(const char *dir, char *path, char *tmp, directory *dp,
                          change_fn change, void *arg)
{
    directory d = *dp;
    (void)dir;
    for (int attempt = 0; attempt < WRITE_ATTEMPTS; attempt++) {
        loaded l;
        size_t need;
        char *body = NULL;
        int fd = -1, ok = 0;

        if (load(dir, &l) != 0)
            return -1;
        if (!l.usable) {
            /* Never overwrite what could not be read: it may be a newer version. */
            unload(&l);
            return -1;
        }
        /* The change first: the host operation it follows (a rename, a
         * delete) has already happened, so pruning first would take a
         * renamed file's entry for a stale one. */
        if (change(&l.file, &l.arena, arg) != 0) {
            unload(&l);
            return -1;
        }
        ameta_prune(&l.file, present, &d);
        need = ameta_format(&l.file, NULL, 0);
        if (need) {
            body = malloc(need);
            ameta_format(&l.file, body, need);
            snprintf(tmp, PATH_CAP, "%s.XXXXXX", path);
            fd = mkstemp(tmp);
            if (fd >= 0) {
                size_t done = 0;
                fchmod(fd, 0644);
                while (done < need) {
                    ssize_t n = write(fd, body + done, need - done);
                    if (n <= 0)
                        break;
                    done += (size_t)n;
                }
                ok = done == need && fsync(fd) == 0;
                close(fd);
            }
        }
        if (need && !ok) {
            if (fd >= 0)
                unlink(tmp);
            free(body);
            unload(&l);
            return -1;
        }
        if (!same_identity(&l, path)) {
            if (need)
                unlink(tmp);
            free(body);
            unload(&l);
            continue;
        }
        if (need)
            ok = rename(tmp, path) == 0;
        else
            ok = unlink(path) == 0 || errno == ENOENT;
        if (!ok && need)
            unlink(tmp);
        free(body);
        unload(&l);
        return ok ? 0 : -1;
    }
    fprintf(stderr, "hostvolume: %s kept changing; metadata not written\n", path);
    return -1;
}

typedef struct {
    const char *name;
    const HVMeta *meta;
} set_args;

static int set_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    const set_args *s = arg;
    size_t len = strlen(s->name);
    ameta_entry *e = ameta_get(f, a, (const unsigned char *)s->name, len);
    char utf8[HV_COMMENT_MAX * 4 + 1];
    if (!e)
        return -1;
    /* Bits above 31 belong to the entry, not to this 32-bit word: keep them. */
    e->prot = (e->has_prot ? e->prot & ~0xFFFFFFFFull : 0) | s->meta->prot;
    e->has_prot = 1;
    ameta_comment_to_utf8(s->meta->comment, utf8, sizeof utf8);
    return ameta_set_comment(e, a, (const unsigned char *)utf8, strlen(utf8));
}

int hv_meta_write(const char *filepath, const HVMeta *m)
{
    char dir[4096];
    const char *name;
    set_args s;
    if (split(filepath, dir, sizeof dir, &name) != 0)
        return -1;
    s.name = name;
    s.meta = m;
    return rewrite(dir, set_entry, &s);
}

typedef struct {
    const unsigned char *name;
    size_t len;
    ameta_entry moved;
    int have;
} take_args;

static int take_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    take_args *t = arg;
    ameta_entry *e = ameta_find(f, t->name, t->len);
    (void)a;
    if (e) {
        t->moved = *e;
        t->have = 1;
        ameta_remove(f, t->name, t->len);
    }
    return 0;
}

typedef struct {
    const take_args *from;
    const char *name;
} put_args;

static int put_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    const put_args *p = arg;
    if (!p->from->have)
        return 0;
    return ameta_copy_entry(f, a, &p->from->moved, (const unsigned char *)p->name,
                            strlen(p->name)) ? 0 : -1;
}

typedef struct {
    const char *from, *to;
} rename_args;

static int rename_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    const rename_args *r = arg;
    return ameta_rename(f, a, (const unsigned char *)r->from, strlen(r->from),
                        (const unsigned char *)r->to, strlen(r->to));
}

int hv_meta_rename(const char *oldpath, const char *newpath)
{
    char olddir[4096], newdir[4096];
    const char *oldname, *newname;
    if (split(oldpath, olddir, sizeof olddir, &oldname) != 0
        || split(newpath, newdir, sizeof newdir, &newname) != 0)
        return -1;
    if (strcmp(olddir, newdir) == 0) {
        rename_args r = { oldname, newname };
        return rewrite(olddir, rename_entry, &r);
    }
    {
        /* Across directories: read the entry from the old .ameta, write it to
         * the new one, then remove it from the old one. */
        loaded l;
        take_args t;
        put_args p;
        ameta_entry *e;
        int rc;
        memset(&t, 0, sizeof t);
        t.name = (const unsigned char *)oldname;
        t.len = strlen(oldname);
        if (load(olddir, &l) != 0)
            return -1;
        e = l.usable ? ameta_find(&l.file, t.name, t.len) : NULL;
        if (!e) {
            unload(&l);
            return 0;
        }
        t.moved = *e;
        t.have = 1;
        p.from = &t;
        p.name = newname;
        rc = rewrite(newdir, put_entry, &p);
        unload(&l);
        if (rc != 0)
            return -1;
        return rewrite(olddir, take_entry, &t);
    }
}

typedef struct {
    const char *name;
} delete_args;

static int delete_entry(ameta_file *f, ameta_arena *a, void *arg)
{
    const delete_args *d = arg;
    (void)a;
    ameta_remove(f, (const unsigned char *)d->name, strlen(d->name));
    return 0;
}

int hv_meta_delete(const char *filepath)
{
    char dir[4096];
    const char *name;
    delete_args d;
    if (split(filepath, dir, sizeof dir, &name) != 0)
        return -1;
    d.name = name;
    return rewrite(dir, delete_entry, &d);
}
