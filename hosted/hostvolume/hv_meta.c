/* hv_meta.c — ".<name>.amimeta" per-file metadata sidecar (R-SIDECAR).
 *
 * Implemented from docs/features/host-volume/spec.md (§Metadata mapping +
 * R-SIDECAR). Independent work — no third-party implementation source was read
 * or consulted; any resemblance is coincidental. The sidecar uses only libc
 * file I/O that the AROS
 * overlay's struct LibCInterface already exposes (open/read/write/close/unlink/
 * rename) [AROS] — no xattr, no new dlsym symbol, no Cocoa. Atomicity is
 * write-temp-then-rename, the POSIX atomic replace [PUB]; the temp is created
 * with mkstemp (unique name + O_EXCL) so concurrent writers to the same sidecar
 * never collide [PUB]. (Note: mkstemp would be added to the overlay's libc
 * symbol set at graft; the spike proves the mechanism host-side.)
 *
 * The filename (".<base>.amimeta", per-file dotfile — NOT a single shared index
 * file), the line-oriented format, the field set, and the "omit when default"
 * rule are OURS (restated in the spec), copied from no reference.
 *
 * Format (line-oriented ASCII, self-describing, forward-compatible):
 *     amimeta 1
 *     prot 0x<hex of the full AROS fib_Protection word>
 *     comment <UTF-8 bytes, percent-escaped past printable ASCII>
 * Unknown lines are ignored on read (forward compatibility). A sidecar is
 * written ONLY when the metadata is non-default; a write of default metadata
 * removes any existing sidecar so the host directory stays clean.
 */
#include "hostvolume.h"

#include <stdio.h>      /* rename() */
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

/* ---- path helpers -------------------------------------------------------- */

size_t hv_sidecar_path(const char *filepath, char *dst, size_t dstcap) {
    /* Split into dir prefix + basename, insert a leading '.', append suffix. */
    const char *slash = strrchr(filepath, '/');
    const char *base  = slash ? slash + 1 : filepath;
    size_t dirlen     = (size_t)(base - filepath);   /* includes the slash */

    /* snprintf-free, fully bounded manual build (no host printf dependency). */
    size_t need = 0;
    #define EMIT(b) do { if (need + 1 < dstcap) dst[need] = (b); need++; } while (0)
    for (size_t i = 0; i < dirlen; i++) EMIT(filepath[i]);
    EMIT('.');
    for (const char *q = base; *q; q++) EMIT(*q);
    static const char suf[] = ".amimeta";
    for (const char *q = suf; *q; q++) EMIT(*q);
    #undef EMIT
    if (dstcap > 0) dst[(need < dstcap) ? need : dstcap - 1] = '\0';
    return need;
}

int hv_is_sidecar_name(const char *name) {
    /* Matches ".*.amimeta" : a leading dot and the ".amimeta" suffix. */
    if (name[0] != '.') return 0;
    size_t len = strlen(name);
    static const char suf[] = ".amimeta";
    size_t slen = sizeof(suf) - 1;
    if (len < slen + 1) return 0;        /* need at least ".X.amimeta" worth */
    return memcmp(name + len - slen, suf, slen) == 0;
}

int hv_meta_is_default(const HVMeta *m) {
    return m->prot == HV_PROT_DEFAULT && m->comment[0] == '\0';
}

/* ---- comment value escaping (printable-ASCII transparent) ----------------
 * The comment is stored on one line, so we escape control chars, newline, the
 * marker '%' itself, and anything >= 0x7F using "%XX" (two hex digits per
 * byte). Printable ASCII (0x20..0x7E except '%') passes through verbatim so a
 * plain-English comment is human-readable in the sidecar. Reversible. */
static int is_transparent(unsigned char c) {
    return c >= 0x20 && c <= 0x7E && c != '%';
}

static size_t esc_comment(const char *in, char *out, size_t cap) {
    static const char hexd[] = "0123456789ABCDEF";
    size_t need = 0;
    #define PUT(b) do { if (need + 1 < cap) out[need] = (b); need++; } while (0)
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (is_transparent(*p)) { PUT((char)*p); }
        else { PUT('%'); PUT(hexd[*p >> 4]); PUT(hexd[*p & 0xF]); }
    }
    #undef PUT
    if (cap > 0) out[(need < cap) ? need : cap - 1] = '\0';
    return need;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void unesc_comment(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '%' && hexval((unsigned char)p[1]) >= 0 &&
                          hexval((unsigned char)p[2]) >= 0) {
            int v = (hexval((unsigned char)p[1]) << 4) | hexval((unsigned char)p[2]);
            if (o + 1 < cap) out[o] = (char)v;
            o++; p += 2;
        } else {
            if (o + 1 < cap) out[o] = *p;
            o++;
        }
    }
    if (cap > 0) out[(o < cap) ? o : cap - 1] = '\0';
}

/* ---- read ---------------------------------------------------------------- */

int hv_meta_read(const char *filepath, HVMeta *out) {
    out->prot = HV_PROT_DEFAULT;
    out->comment[0] = '\0';

    char path[4096];
    hv_sidecar_path(filepath, path, sizeof path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return 0;   /* absent -> defaults, not an error */
        return -1;
    }

    char buf[8192];
    ssize_t total = 0, r;
    while ((r = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0) {
        total += r;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    close(fd);
    if (r < 0) return -1;
    buf[total] = '\0';

    int saw_header = 0;
    char *line = buf, *nl;
    do {
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        /* split "key value" */
        char *sp = strchr(line, ' ');
        const char *key = line;
        const char *val = "";
        if (sp) { *sp = '\0'; val = sp + 1; }

        if (strcmp(key, "amimeta") == 0) {
            saw_header = 1;
        } else if (strcmp(key, "prot") == 0) {
            out->prot = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "comment") == 0) {
            char tmp[HV_COMMENT_MAX * 3 + 1];
            /* val is UTF-8/percent-escaped; un-escape to raw bytes. The AROS
             * comment is Latin-1; for the round-trip test we store and recover
             * the exact bytes handed in. */
            unesc_comment(val, tmp, sizeof tmp);
            strncpy(out->comment, tmp, HV_COMMENT_MAX);
            out->comment[HV_COMMENT_MAX] = '\0';
        }
        /* unknown keys ignored (forward compatible) */
        line = nl ? nl + 1 : NULL;
    } while (line && *line);

    if (!saw_header) return -1;          /* not our file / corrupt */
    return 1;
}

/* ---- write (atomic: temp + rename) --------------------------------------- */

static size_t append_str(char *buf, size_t off, size_t cap, const char *s) {
    while (*s && off + 1 < cap) buf[off++] = *s++;
    return off;
}

static size_t append_hex32(char *buf, size_t off, size_t cap, uint32_t v) {
    static const char hexd[] = "0123456789ABCDEF";
    off = append_str(buf, off, cap, "0x");
    char d[8]; int n = 0;
    if (v == 0) { if (off + 1 < cap) buf[off++] = '0'; return off; }
    while (v) { d[n++] = hexd[v & 0xF]; v >>= 4; }
    while (n-- > 0 && off + 1 < cap) buf[off++] = d[n];
    return off;
}

int hv_meta_write(const char *filepath, const HVMeta *m) {
    char path[4096];
    hv_sidecar_path(filepath, path, sizeof path);

    /* Default metadata: ensure no sidecar lingers, then succeed. */
    if (hv_meta_is_default(m)) {
        if (unlink(path) != 0 && errno != ENOENT) return -1;
        return 0;
    }

    /* Compose the record body. */
    char body[HV_COMMENT_MAX * 3 + 256];
    size_t off = 0;
    off = append_str(body, off, sizeof body, "amimeta 1\n");
    off = append_str(body, off, sizeof body, "prot ");
    off = append_hex32(body, off, sizeof body, m->prot);
    off = append_str(body, off, sizeof body, "\n");
    if (m->comment[0]) {
        char esc[HV_COMMENT_MAX * 3 + 1];
        esc_comment(m->comment, esc, sizeof esc);
        off = append_str(body, off, sizeof body, "comment ");
        off = append_str(body, off, sizeof body, esc);
        off = append_str(body, off, sizeof body, "\n");
    }

    /* Atomic replace: write a temp in the SAME directory, fsync, rename over.
     * The temp name MUST be unique per writer so two concurrent writes to the
     * same sidecar never collide (a pid-only name is NOT enough: the same
     * process writing the same file twice, or any racing writer, would reuse
     * the name and O_TRUNC each other's data). mkstemp() generates a unique
     * name AND opens it O_CREAT|O_EXCL atomically, closing that race. */
    char tmp[4096 + 16];
    /* temp template = ".<base>.amimeta.tmpXXXXXX" in the SAME dir as `path`
     * (same dir guarantees the rename is an atomic in-place replace). */
    size_t pl = strlen(path);
    static const char tmpl_suf[] = ".tmpXXXXXX";   /* 6 X's for mkstemp */
    if (pl + sizeof(tmpl_suf) >= sizeof tmp) return -1;
    memcpy(tmp, path, pl);
    memcpy(tmp + pl, tmpl_suf, sizeof tmpl_suf);    /* includes the NUL */

    int fd = mkstemp(tmp);                          /* unique + O_EXCL create */
    if (fd < 0) return -1;
    fchmod(fd, 0644);                                /* mkstemp makes 0600; the
                                                     * sidecar should match a
                                                     * normal 0644 data file */
    size_t wr = 0;
    while (wr < off) {
        ssize_t n = write(fd, body + wr, off - wr);
        if (n < 0) { close(fd); unlink(tmp); return -1; }
        wr += (size_t)n;
    }
    fsync(fd);
    close(fd);

    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}
