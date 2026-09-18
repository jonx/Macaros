/* ameta.c — the .ameta per-directory Amiga metadata format, pure core.
 *
 * See ameta.h and aros-next docs/features/file-metadata/ameta.md. This file
 * builds unchanged in the hosted tests and, copied as emul_ameta.c, in the
 * AROS emul-handler; keep the two copies identical.
 */
#include "ameta.h"

#include <string.h>

/* ---- arena ---------------------------------------------------------------- */

static void *arena_alloc(ameta_arena *a, size_t size)
{
    size_t start = (a->used + 7u) & ~(size_t)7u;
    if (start > a->cap || size > a->cap - start)
        return NULL;
    a->used = start + size;
    return a->base + start;
}

static unsigned char *arena_copy(ameta_arena *a, const void *src, size_t len)
{
    unsigned char *p = arena_alloc(a, len ? len : 1);
    if (p && len)
        memcpy(p, src, len);
    return p;
}

/* ---- escaping ------------------------------------------------------------- */

static const char hexdigits[] = "0123456789ABCDEF";

static int escaped_byte(unsigned char b)
{
    return b == '%' || b <= 0x20 || b >= 0x7F;
}

size_t ameta_escape(const unsigned char *in, size_t len, char *out, size_t cap)
{
    size_t need = 0, i;
#define PUT(c) do { if (need < cap) out[need] = (char)(c); need++; } while (0)
    for (i = 0; i < len; i++) {
        if (escaped_byte(in[i])) {
            PUT('%');
            PUT(hexdigits[in[i] >> 4]);
            PUT(hexdigits[in[i] & 0xF]);
        } else {
            PUT(in[i]);
        }
    }
#undef PUT
    return need;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

long ameta_unescape(const char *in, size_t len, unsigned char *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < len) {
        if (in[i] == '%') {
            int hi, lo;
            if (len - i < 3)
                return -1;
            hi = hexval((unsigned char)in[i + 1]);
            lo = hexval((unsigned char)in[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;
            if (o < cap) out[o] = (unsigned char)(hi << 4 | lo);
            o++;
            i += 3;
        } else {
            if (o < cap) out[o] = (unsigned char)in[i];
            o++;
            i++;
        }
    }
    return (long)o;
}

/* The escaped form of a name, one byte at a time, for canonical ordering. */
typedef struct {
    const unsigned char *name;
    size_t len, at;
    char pending[2];
    int npending;
} esc_stream;

static int esc_next(esc_stream *s)
{
    unsigned char b;
    if (s->npending) {
        char c = s->pending[0];
        s->pending[0] = s->pending[1];
        s->npending--;
        return (unsigned char)c;
    }
    if (s->at >= s->len)
        return -1;
    b = s->name[s->at++];
    if (!escaped_byte(b))
        return b;
    s->pending[0] = hexdigits[b >> 4];
    s->pending[1] = hexdigits[b & 0xF];
    s->npending = 2;
    return '%';
}

static int escaped_compare(const ameta_entry *a, const ameta_entry *b)
{
    esc_stream x = { a->name, a->name_len, 0, { 0, 0 }, 0 };
    esc_stream y = { b->name, b->name_len, 0, { 0, 0 }, 0 };
    for (;;) {
        int p = esc_next(&x), q = esc_next(&y);
        if (p != q)
            return p < q ? -1 : 1;
        if (p < 0)
            return 0;
    }
}

/* ---- parsing -------------------------------------------------------------- */

static int valid_utf8(const unsigned char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        size_t n;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        else return 0;
        if (len - i <= n)
            return 0;
        for (size_t k = 1; k <= n; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF))
            || (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += n + 1;
    }
    return 1;
}

static int parse_prot(const char *v, size_t len, uint64_t *out, const char **why)
{
    uint64_t word = 0;
    size_t i;
    if (len < 3 || v[0] != '0' || v[1] != 'x') {
        *why = "bad-number";
        return -1;
    }
    if (len - 2 > 16) {
        *why = "prot-too-long";
        return -1;
    }
    for (i = 2; i < len; i++) {
        int h = hexval((unsigned char)v[i]);
        if (h < 0) {
            *why = "bad-number";
            return -1;
        }
        word = word << 4 | (uint64_t)h;
    }
    *out = word;
    return 0;
}

static int parse_id(const char *v, size_t len, uint32_t *out)
{
    uint64_t n = 0;
    size_t i;
    if (len == 0)
        return -1;
    for (i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9')
            return -1;
        n = n * 10 + (uint64_t)(v[i] - '0');
        if (n > 0xFFFFFFFFull)
            return -1;
    }
    *out = (uint32_t)n;
    return 0;
}

static ameta_entry *add_entry(ameta_file *f, ameta_arena *a)
{
    if (f->count == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 8;
        ameta_entry *grown = arena_alloc(a, cap * sizeof *grown);
        if (!grown)
            return NULL;
        if (f->count)
            memcpy(grown, f->entries, f->count * sizeof *grown);
        f->entries = grown;
        f->cap = cap;
    }
    memset(&f->entries[f->count], 0, sizeof f->entries[0]);
    return &f->entries[f->count++];
}

static int add_unknown(ameta_entry *e, ameta_arena *a, const char *text, size_t len)
{
    ameta_line *grown = arena_alloc(a, (e->unknown_count + 1) * sizeof *grown);
    if (!grown)
        return -1;
    if (e->unknown_count)
        memcpy(grown, e->unknown, e->unknown_count * sizeof *grown);
    grown[e->unknown_count].text = (const char *)arena_copy(a, text, len);
    grown[e->unknown_count].len = len;
    if (!grown[e->unknown_count].text)
        return -1;
    e->unknown = grown;
    e->unknown_count++;
    return 0;
}

static int keyis(const char *key, size_t klen, const char *want)
{
    size_t n = strlen(want);
    return klen == n && memcmp(key, want, n) == 0;
}

static void report(ameta_report_fn fn, void *ctx, unsigned line, const char *why)
{
    if (fn)
        fn(ctx, line, why);
}

int ameta_parse(const char *buf, size_t len, ameta_arena *a, ameta_file *out,
                ameta_report_fn rep, void *ctx)
{
    size_t at = 0;
    unsigned number = 0;
    ameta_entry *current = NULL;

    memset(out, 0, sizeof *out);
    if (len == 0)
        return AMETA_OK;
    if (len > AMETA_MAX_FILE) {
        report(rep, ctx, 0, "file-too-large");
        return AMETA_UNUSABLE;
    }
    while (at < len) {
        const char *line = buf + at;
        const char *nl = memchr(line, '\n', len - at);
        size_t llen = nl ? (size_t)(nl - line) : len - at;
        const char *key, *val, *sp;
        size_t klen, vlen;
        at += llen + (nl ? 1 : 0);
        number++;
        if (llen && line[llen - 1] == '\r')
            llen--;
        if (number == 1) {
            if (llen != 7 || memcmp(line, "ameta 1", 7) != 0) {
                report(rep, ctx, 1, "bad-header");
                return AMETA_UNUSABLE;
            }
            continue;
        }
        if (llen > AMETA_MAX_LINE) {
            report(rep, ctx, number, "line-too-long");
            continue;
        }
        if (!valid_utf8((const unsigned char *)line, llen)) {
            report(rep, ctx, number, "not-utf8");
            continue;
        }
        sp = memchr(line, ' ', llen);
        key = line;
        klen = sp ? (size_t)(sp - line) : llen;
        val = sp ? sp + 1 : line + llen;
        vlen = sp ? llen - klen - 1 : 0;

        if (keyis(key, klen, "file")) {
            unsigned char *name = arena_alloc(a, vlen ? vlen : 1);
            long nlen;
            ameta_entry *existing;
            if (!name)
                return AMETA_NOMEM;
            nlen = ameta_unescape(val, vlen, name, vlen);
            if (nlen < 0) {
                report(rep, ctx, number, "malformed-escape");
                current = NULL;
                continue;
            }
            if (nlen == 0 || (nlen == 1 && name[0] == '.')
                || (nlen == 2 && name[0] == '.' && name[1] == '.')
                || memchr(name, '/', (size_t)nlen)) {
                report(rep, ctx, number, "bad-name");
                current = NULL;
                continue;
            }
            existing = ameta_find(out, name, (size_t)nlen);
            if (existing) {
                /* The later section applies, in place of the earlier one. */
                report(rep, ctx, number, "duplicate");
                memset(existing, 0, sizeof *existing);
                current = existing;
            } else {
                current = add_entry(out, a);
                if (!current)
                    return AMETA_NOMEM;
            }
            current->name = name;
            current->name_len = (size_t)nlen;
        } else if (!current) {
            report(rep, ctx, number, "key-before-file");
        } else if (keyis(key, klen, "prot")) {
            const char *why = NULL;
            uint64_t word;
            if (parse_prot(val, vlen, &word, &why) == 0) {
                current->has_prot = 1;
                current->prot = word;
            } else {
                report(rep, ctx, number, why);
            }
        } else if (keyis(key, klen, "comment")) {
            unsigned char *text = arena_alloc(a, vlen ? vlen : 1);
            long tlen;
            if (!text)
                return AMETA_NOMEM;
            tlen = ameta_unescape(val, vlen, text, vlen);
            if (tlen < 0) {
                report(rep, ctx, number, "malformed-escape");
            } else {
                current->comment = text;
                current->comment_len = (size_t)tlen;
            }
        } else if (keyis(key, klen, "uid") || keyis(key, klen, "gid")) {
            uint32_t id;
            if (parse_id(val, vlen, &id) != 0) {
                report(rep, ctx, number, "bad-number");
            } else if (key[0] == 'u') {
                current->has_uid = 1;
                current->uid = id;
            } else {
                current->has_gid = 1;
                current->gid = id;
            }
        } else {
            if (add_unknown(current, a, line, llen) != 0)
                return AMETA_NOMEM;
        }
    }
    if (number == 0) {
        report(rep, ctx, 1, "bad-header");
        return AMETA_UNUSABLE;
    }
    return AMETA_OK;
}

/* ---- editing -------------------------------------------------------------- */

ameta_entry *ameta_find(ameta_file *f, const unsigned char *name, size_t len)
{
    size_t i;
    for (i = 0; i < f->count; i++)
        if (f->entries[i].name_len == len && memcmp(f->entries[i].name, name, len) == 0)
            return &f->entries[i];
    return NULL;
}

ameta_entry *ameta_get(ameta_file *f, ameta_arena *a, const unsigned char *name, size_t len)
{
    ameta_entry *e = ameta_find(f, name, len);
    unsigned char *copy;
    if (e)
        return e;
    copy = arena_copy(a, name, len);
    if (!copy)
        return NULL;
    e = add_entry(f, a);
    if (!e)
        return NULL;
    e->name = copy;
    e->name_len = len;
    return e;
}

int ameta_set_comment(ameta_entry *e, ameta_arena *a, const unsigned char *comment, size_t len)
{
    unsigned char *copy = arena_copy(a, comment, len);
    if (!copy)
        return -1;
    e->comment = copy;
    e->comment_len = len;
    return 0;
}

void ameta_remove(ameta_file *f, const unsigned char *name, size_t len)
{
    ameta_entry *e = ameta_find(f, name, len);
    size_t i;
    if (!e)
        return;
    i = (size_t)(e - f->entries);
    memmove(&f->entries[i], &f->entries[i + 1], (f->count - i - 1) * sizeof f->entries[0]);
    f->count--;
}

ameta_entry *ameta_copy_entry(ameta_file *f, ameta_arena *a, const ameta_entry *from,
                              const unsigned char *name, size_t len)
{
    ameta_entry copy = *from;
    ameta_entry *e;
    size_t i;
    copy.name = arena_copy(a, name, len);
    copy.name_len = len;
    copy.comment = arena_copy(a, from->comment, from->comment_len);
    copy.unknown = from->unknown_count
                       ? arena_alloc(a, from->unknown_count * sizeof *copy.unknown) : NULL;
    if (!copy.name || !copy.comment || (from->unknown_count && !copy.unknown))
        return NULL;
    for (i = 0; i < from->unknown_count; i++) {
        copy.unknown[i].len = from->unknown[i].len;
        copy.unknown[i].text = (const char *)arena_copy(a, from->unknown[i].text,
                                                        from->unknown[i].len);
        if (!copy.unknown[i].text)
            return NULL;
    }
    ameta_remove(f, name, len);
    e = add_entry(f, a);
    if (!e)
        return NULL;
    *e = copy;
    return e;
}

int ameta_rename(ameta_file *f, ameta_arena *a, const unsigned char *from, size_t from_len,
                 const unsigned char *to, size_t to_len)
{
    ameta_entry *e = ameta_find(f, from, from_len);
    unsigned char *copy;
    if (!e)
        return 0;
    ameta_remove(f, to, to_len);
    e = ameta_find(f, from, from_len);
    copy = arena_copy(a, to, to_len);
    if (!copy)
        return -1;
    e->name = copy;
    e->name_len = to_len;
    return 0;
}

void ameta_prune(ameta_file *f, ameta_exists_fn exists, void *ctx)
{
    size_t i = 0;
    while (i < f->count) {
        if (exists(ctx, f->entries[i].name, f->entries[i].name_len)) {
            i++;
        } else {
            memmove(&f->entries[i], &f->entries[i + 1],
                    (f->count - i - 1) * sizeof f->entries[0]);
            f->count--;
        }
    }
}

int ameta_entry_is_default(const ameta_entry *e)
{
    return (!e->has_prot || (e->prot & ~AMETA_RECORD_MASK) == 0) && e->comment_len == 0
           && !e->has_uid && !e->has_gid && e->unknown_count == 0;
}

/* ---- writing -------------------------------------------------------------- */

static size_t put_text(char *out, size_t cap, size_t at, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (at + i < cap)
            out[at + i] = s[i];
    return at + len;
}

static size_t put_str(char *out, size_t cap, size_t at, const char *s)
{
    return put_text(out, cap, at, s, strlen(s));
}

static size_t put_hex(char *out, size_t cap, size_t at, uint64_t word)
{
    int digits = word > 0xFFFFFFFFull ? 16 : 8, i;
    at = put_str(out, cap, at, "0x");
    for (i = digits - 1; i >= 0; i--) {
        char c = hexdigits[(word >> (4 * i)) & 0xF];
        at = put_text(out, cap, at, &c, 1);
    }
    return at;
}

static size_t put_decimal(char *out, size_t cap, size_t at, uint32_t n)
{
    char digits[10];
    int k = 0;
    do {
        digits[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (k--)
        at = put_text(out, cap, at, &digits[k], 1);
    return at;
}

size_t ameta_format(ameta_file *f, char *out, size_t cap)
{
    size_t i, j, at = 0, kept = 0;

    /* Canonical order: bytewise on the escaped names. */
    for (i = 1; i < f->count; i++) {
        ameta_entry moving = f->entries[i];
        j = i;
        while (j > 0 && escaped_compare(&f->entries[j - 1], &moving) > 0) {
            f->entries[j] = f->entries[j - 1];
            j--;
        }
        f->entries[j] = moving;
    }
    for (i = 0; i < f->count; i++)
        if (!ameta_entry_is_default(&f->entries[i]))
            kept++;
    if (kept == 0)
        return 0;

    at = put_str(out, cap, at, "ameta 1\n");
    for (i = 0; i < f->count; i++) {
        const ameta_entry *e = &f->entries[i];
        if (ameta_entry_is_default(e))
            continue;
        at = put_str(out, cap, at, "file ");
        at += ameta_escape(e->name, e->name_len, at < cap ? out + at : out, at < cap ? cap - at : 0);
        at = put_str(out, cap, at, "\n");
        if (e->has_prot && (e->prot & ~AMETA_RECORD_MASK)) {
            at = put_str(out, cap, at, "prot ");
            at = put_hex(out, cap, at, e->prot);
            at = put_str(out, cap, at, "\n");
        }
        if (e->comment_len) {
            at = put_str(out, cap, at, "comment ");
            at += ameta_escape(e->comment, e->comment_len, at < cap ? out + at : out,
                               at < cap ? cap - at : 0);
            at = put_str(out, cap, at, "\n");
        }
        if (e->has_uid) {
            at = put_str(out, cap, at, "uid ");
            at = put_decimal(out, cap, at, e->uid);
            at = put_str(out, cap, at, "\n");
        }
        if (e->has_gid) {
            at = put_str(out, cap, at, "gid ");
            at = put_decimal(out, cap, at, e->gid);
            at = put_str(out, cap, at, "\n");
        }
        for (j = 0; j < e->unknown_count; j++) {
            at = put_text(out, cap, at, e->unknown[j].text, e->unknown[j].len);
            at = put_str(out, cap, at, "\n");
        }
    }
    return at;
}

/* ---- the comment between AROS Latin-1 and UTF-8 ----------------------------- *
 * Every Latin-1 byte is its own code point, '%' included. A character beyond
 * Latin-1 reaches AROS as "%uXXXX" and comes back as itself, so an AROS
 * program that only rewrites what it read loses nothing. */

size_t ameta_comment_to_utf8(const char *latin1, char *out, size_t cap)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)latin1;
#define PUT(c) do { if (o + 1 < cap) out[o] = (char)(c); o++; } while (0)
    while (*p) {
        unsigned long cp = *p++;
        if (cp == '%' && p[0] == 'u') {
            unsigned long v = 0;
            int k, h = 0;
            for (k = 1; k <= 4; k++) {
                int c = p[k];
                h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
                if (h < 0)
                    break;
                v = v << 4 | (unsigned long)h;
            }
            if (h >= 0 && v > 0xFF) {
                cp = v;
                p += 5;
            }
        }
        if (cp < 0x80) {
            PUT(cp);
        } else if (cp < 0x800) {
            PUT(0xC0 | cp >> 6);
            PUT(0x80 | (cp & 0x3F));
        } else {
            PUT(0xE0 | cp >> 12);
            PUT(0x80 | ((cp >> 6) & 0x3F));
            PUT(0x80 | (cp & 0x3F));
        }
    }
#undef PUT
    if (cap)
        out[o < cap ? o : cap - 1] = '\0';
    return o;
}

void ameta_comment_to_latin1(const unsigned char *utf8, size_t len, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i = 0, o = 0;
#define PUT(c) do { if (o + 1 < cap) out[o++] = (char)(c); } while (0)
    while (i < len) {
        unsigned long cp = utf8[i];
        size_t n = cp < 0x80 ? 0 : cp < 0xE0 ? 1 : cp < 0xF0 ? 2 : 3;
        if (n) {
            cp &= 0x3F >> n;
            for (size_t k = 1; k <= n && i + k < len; k++)
                cp = cp << 6 | (utf8[i + k] & 0x3F);
        }
        i += n + 1;
        if (cp <= 0xFF) {
            PUT(cp);
        } else if (cp <= 0xFFFF) {
            PUT('%'); PUT('u');
            PUT(hex[cp >> 12 & 0xF]); PUT(hex[cp >> 8 & 0xF]);
            PUT(hex[cp >> 4 & 0xF]); PUT(hex[cp & 0xF]);
        } else {
            PUT('?');
        }
    }
#undef PUT
    if (cap)
        out[o] = '\0';
}
