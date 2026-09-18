/* ameta_corpus_test.c — the C .ameta core against the reference corpus.
 *
 *   build/host-ameta [corpus-cases-dir]
 *
 * Every case of the corpus (ameta-corpus/cases, see its README) is read and,
 * when it has an operation, written; the results must equal the case's
 * expected files, exactly as aros-next's check.py requires of the reference
 * implementation. Prints "[AMETA] PASS" when every case holds.
 */
#include "ameta.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *dir, const char *name, size_t *len)
{
    char path[1024];
    FILE *f;
    char *buf;
    long size;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    *len = (size_t)size;
    return buf;
}

/* ---- text builder ---------------------------------------------------------- */

typedef struct {
    char *text;
    size_t len, cap;
} text;

static void add(text *t, const char *s, size_t n)
{
    if (t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        t->text = realloc(t->text, t->cap);
    }
    memcpy(t->text + t->len, s, n);
    t->len += n;
    t->text[t->len] = '\0';
}

static void adds(text *t, const char *s)
{
    add(t, s, strlen(s));
}

static void add_escaped(text *t, const unsigned char *s, size_t n)
{
    size_t need = ameta_escape(s, n, NULL, 0);
    char *buf = malloc(need + 1);
    ameta_escape(s, n, buf, need);
    add(t, buf, need);
    free(buf);
}

static void add_prot(text *t, uint64_t word)
{
    char buf[32];
    if (word > 0xFFFFFFFFull)
        snprintf(buf, sizeof buf, "0x%016llX", (unsigned long long)word);
    else
        snprintf(buf, sizeof buf, "0x%08llX", (unsigned long long)word);
    adds(t, buf);
}

/* ---- the case's directory ------------------------------------------------- */

typedef struct {
    unsigned char name[256];
    size_t len;
} present;

typedef struct {
    present *names;
    size_t count;
} listing;

static int in_listing(void *ctx, const unsigned char *name, size_t len)
{
    listing *l = ctx;
    for (size_t i = 0; i < l->count; i++)
        if (l->names[i].len == len && memcmp(l->names[i].name, name, len) == 0)
            return 1;
    return 0;
}

/* ---- reports --------------------------------------------------------------- */

typedef struct {
    text lines;
    int unusable;
} reports;

static void on_report(void *ctx, unsigned line, const char *why)
{
    reports *r = ctx;
    char buf[64];
    snprintf(buf, sizeof buf, "report %u %s\n", line, why);
    adds(&r->lines, buf);
    (void)line;
}

/* The same description check.py gives: entries, then stale names, then reports. */
static void describe(text *out, ameta_file *f, listing *present_names, reports *r)
{
    size_t i;
    /* Order by escaped name, as the writer does. */
    ameta_file sorted = *f;
    char scratch[1];
    ameta_format(&sorted, scratch, 0);
    for (i = 0; i < f->count; i++) {
        ameta_entry *e = &f->entries[i];
        if (!in_listing(present_names, e->name, e->name_len))
            continue;
        adds(out, "entry ");
        add_escaped(out, e->name, e->name_len);
        if (e->has_prot) {
            adds(out, " prot ");
            add_prot(out, e->prot);
        }
        if (e->comment_len) {
            adds(out, " comment ");
            add_escaped(out, e->comment, e->comment_len);
        }
        if (e->has_uid) {
            char buf[24];
            snprintf(buf, sizeof buf, " uid %u", e->uid);
            adds(out, buf);
        }
        if (e->has_gid) {
            char buf[24];
            snprintf(buf, sizeof buf, " gid %u", e->gid);
            adds(out, buf);
        }
        adds(out, "\n");
    }
    for (i = 0; i < f->count; i++) {
        ameta_entry *e = &f->entries[i];
        if (in_listing(present_names, e->name, e->name_len))
            continue;
        adds(out, "stale ");
        add_escaped(out, e->name, e->name_len);
        adds(out, "\n");
    }
    if (r->lines.len)
        add(out, r->lines.text, r->lines.len);
}

static unsigned char *unescape_word(const char *word, size_t *len)
{
    size_t n = strlen(word);
    unsigned char *out = malloc(n + 1);
    long got = ameta_unescape(word, n, out, n);
    *len = got < 0 ? 0 : (size_t)got;
    return out;
}

static int apply(ameta_file *f, ameta_arena *a, char *operation)
{
    char *line = strtok(operation, "\n");
    for (; line; line = strtok(NULL, "\n")) {
        char *words[5] = { 0 };
        int n = 0;
        char *save, *w = strtok_r(line, " ", &save);
        while (w && n < 5) {
            words[n++] = w;
            w = strtok_r(NULL, " ", &save);
        }
        if (n == 0)
            continue;
        if (strcmp(words[0], "set") == 0 && n >= 4) {
            size_t len, vlen;
            unsigned char *name = unescape_word(words[1], &len);
            ameta_entry *e = ameta_get(f, a, name, len);
            free(name);
            if (!e)
                return -1;
            if (strcmp(words[2], "prot") == 0) {
                e->has_prot = 1;
                e->prot = strtoull(words[3], NULL, 16);
            } else if (strcmp(words[2], "comment") == 0) {
                unsigned char *value = unescape_word(words[3], &vlen);
                ameta_set_comment(e, a, value, vlen);
                free(value);
            } else if (strcmp(words[2], "uid") == 0) {
                e->has_uid = 1;
                e->uid = (uint32_t)strtoul(words[3], NULL, 10);
            } else if (strcmp(words[2], "gid") == 0) {
                e->has_gid = 1;
                e->gid = (uint32_t)strtoul(words[3], NULL, 10);
            }
        } else if (strcmp(words[0], "delete") == 0 && n >= 2) {
            size_t len;
            unsigned char *name = unescape_word(words[1], &len);
            ameta_remove(f, name, len);
            free(name);
        } else if (strcmp(words[0], "rename") == 0 && n >= 3) {
            size_t from_len, to_len;
            unsigned char *from = unescape_word(words[1], &from_len);
            unsigned char *to = unescape_word(words[2], &to_len);
            ameta_rename(f, a, from, from_len, to, to_len);
            free(from);
            free(to);
        }
    }
    return 0;
}

static int run_case(const char *dir, const char *name)
{
    char path[1024];
    size_t len = 0, elen = 0, rlen = 0, olen = 0, xlen = 0, oclen = 0;
    char *input, *entries, *expected_read, *operation, *expected, *outcome;
    listing names = { 0 };
    reports r = { { 0 }, 0 };
    text got = { 0 };
    ameta_file f;
    ameta_arena arena;
    int status, ok = 1;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    input = slurp(path, "input.ameta", &len);
    entries = slurp(path, "entries", &elen);
    expected_read = slurp(path, "read", &rlen);
    operation = slurp(path, "operation", &olen);
    expected = slurp(path, "expected.ameta", &xlen);
    outcome = slurp(path, "outcome", &oclen);

    if (entries) {
        char *save, *line = strtok_r(entries, "\n", &save);
        names.names = calloc(64, sizeof *names.names);
        for (; line; line = strtok_r(NULL, "\n", &save)) {
            long n = ameta_unescape(line, strlen(line), names.names[names.count].name, 255);
            names.names[names.count++].len = n < 0 ? 0 : (size_t)n;
        }
    }

    arena.cap = len * 4 + 65536;
    arena.base = malloc(arena.cap);
    arena.used = 0;
    status = ameta_parse(input ? input : "", len, &arena, &f, on_report, &r);
    if (status == AMETA_NOMEM) {
        printf("FAIL  %s: arena too small\n", name);
        return 0;
    }
    if (status == AMETA_UNUSABLE) {
        /* check.py prints "ignored-file <reason>" from the single report. */
        char *reason = r.lines.text ? strchr(r.lines.text + 7, ' ') : NULL;
        adds(&got, "ignored-file");
        if (reason)
            adds(&got, reason);
    } else {
        describe(&got, &f, &names, &r);
    }
    if (strcmp(got.text ? got.text : "", expected_read ? expected_read : "") != 0) {
        printf("FAIL  %s: read\n--- expected\n%s--- got\n%s", name,
               expected_read ? expected_read : "", got.text ? got.text : "");
        ok = 0;
    }

    if (operation) {
        const char *want = outcome ? outcome : "write\n";
        char result_kind[16];
        char *written = NULL;
        size_t need = 0;
        if (status == AMETA_UNUSABLE) {
            strcpy(result_kind, "refuse\n");
        } else {
            /* Stale entries go first, against the directory as it was read;
             * the operation then acts on what is left, as check.py does. */
            ameta_prune(&f, in_listing, &names);
            apply(&f, &arena, operation);
            need = ameta_format(&f, NULL, 0);
            if (need == 0) {
                strcpy(result_kind, "delete\n");
            } else {
                written = malloc(need + 1);
                ameta_format(&f, written, need);
                written[need] = '\0';
                strcpy(result_kind, "write\n");
            }
        }
        if (strcmp(result_kind, want) != 0) {
            printf("FAIL  %s: outcome %s, expected %s", name, result_kind, want);
            ok = 0;
        } else if (written && strcmp(written, expected ? expected : "") != 0) {
            printf("FAIL  %s: write\n--- expected\n%s--- got\n%s", name,
                   expected ? expected : "", written);
            ok = 0;
        }
        free(written);
    }
    if (ok)
        printf("ok    %s\n", name);
    free(input);
    free(entries);
    free(expected_read);
    free(operation);
    free(expected);
    free(outcome);
    free(names.names);
    free(arena.base);
    free(got.text);
    free(r.lines.text);
    return ok;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "hosted/hostvolume/ameta-corpus/cases";
    DIR *d = opendir(dir);
    struct dirent *de;
    char *cases[256];
    int count = 0, failed = 0;
    if (!d) {
        printf("[AMETA] FAIL: cannot open %s\n", dir);
        return 1;
    }
    while ((de = readdir(d)) && count < 256)
        if (de->d_name[0] != '.')
            cases[count++] = strdup(de->d_name);
    closedir(d);
    qsort(cases, (size_t)count, sizeof cases[0], by_name);
    for (int i = 0; i < count; i++) {
        failed += !run_case(dir, cases[i]);
        free(cases[i]);
    }
    if (count == 0 || failed) {
        printf("[AMETA] FAIL: %d of %d cases\n", failed, count);
        return 1;
    }
    printf("[AMETA] PASS: %d cases\n", count);
    return 0;
}
