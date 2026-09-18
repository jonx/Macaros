/* ameta.h — the .ameta per-directory Amiga metadata format, pure core.
 *
 * The format is aros-next docs/features/file-metadata/ameta.md; this core
 * parses, edits and writes it and does no I/O. It uses only <string.h> and
 * <stdint.h> and allocates nothing: every allocation comes from an arena the
 * caller supplies, so the same file builds in the hosted tests and in the
 * AROS emul-handler, where it is copied as emul_ameta.c.
 */
#ifndef AMETA_H
#define AMETA_H

#include <stddef.h>
#include <stdint.h>

/* Bits a live host volume takes from the host mode, and the bits a writer
 * leaves out of the record (owner Execute, group and other rwx). */
#define AMETA_HOST_MASK    0xEE0Eull
#define AMETA_RECORD_MASK  0xEE02ull
#define AMETA_MAX_LINE     4096u
#define AMETA_MAX_FILE     (1u << 20)

typedef struct {
    unsigned char *base;
    size_t cap;
    size_t used;
} ameta_arena;

typedef struct {
    const char *text;       /* "key value" of an unknown key, kept verbatim */
    size_t len;
} ameta_line;

typedef struct {
    unsigned char *name;    /* unescaped UTF-8 bytes, not NUL-terminated */
    size_t name_len;
    int has_prot;
    uint64_t prot;
    unsigned char *comment; /* unescaped UTF-8 bytes */
    size_t comment_len;
    int has_uid, has_gid;
    uint32_t uid, gid;
    ameta_line *unknown;
    size_t unknown_count;
} ameta_entry;

typedef struct {
    ameta_entry *entries;
    size_t count, cap;
} ameta_file;

/* A malformed line, or a whole file that cannot be read (line 0 or 1). */
typedef void (*ameta_report_fn)(void *ctx, unsigned line, const char *reason);
/* Whether the directory holds an entry of this name. */
typedef int (*ameta_exists_fn)(void *ctx, const unsigned char *name, size_t len);

enum {
    AMETA_OK = 0,
    AMETA_UNUSABLE = 1,     /* bad header or too large: ignore, never overwrite */
    AMETA_NOMEM = -1        /* the arena was too small */
};

/* Parse a whole .ameta. An empty buffer is an empty file (no .ameta). */
int ameta_parse(const char *buf, size_t len, ameta_arena *arena, ameta_file *out,
                ameta_report_fn report, void *ctx);

ameta_entry *ameta_find(ameta_file *f, const unsigned char *name, size_t len);
/* Find the entry, or add an empty one; NULL when the arena is exhausted. */
ameta_entry *ameta_get(ameta_file *f, ameta_arena *arena, const unsigned char *name, size_t len);
int ameta_set_comment(ameta_entry *e, ameta_arena *arena, const unsigned char *comment, size_t len);
void ameta_remove(ameta_file *f, const unsigned char *name, size_t len);
/* Copy an entry, with everything it carries, under a name of this file;
 * an existing entry of that name is replaced. For a move between directories. */
ameta_entry *ameta_copy_entry(ameta_file *f, ameta_arena *arena, const ameta_entry *from,
                              const unsigned char *name, size_t len);
int ameta_rename(ameta_file *f, ameta_arena *arena, const unsigned char *from, size_t from_len,
                 const unsigned char *to, size_t to_len);
/* Drop the entries that name nothing present in the directory. */
void ameta_prune(ameta_file *f, ameta_exists_fn exists, void *ctx);

/* An entry carrying nothing worth a line: a writer drops it. */
int ameta_entry_is_default(const ameta_entry *e);

/* The canonical file, into out; returns the bytes it needs (which may exceed
 * cap), and 0 when no entry is left and the file must not exist. */
size_t ameta_format(ameta_file *f, char *out, size_t cap);

/* Percent-escape bytes as the format does; returns the bytes needed. */
size_t ameta_escape(const unsigned char *in, size_t len, char *out, size_t cap);
/* Undo it; -1 on a malformed escape, else the length written. */
long ameta_unescape(const char *in, size_t len, unsigned char *out, size_t cap);

/* A classic AROS comment is Latin-1: every byte is its own code point, '%'
 * included, and a character beyond Latin-1 travels as "%uXXXX" and comes back
 * as itself. These convert between that and the UTF-8 of the format. */
size_t ameta_comment_to_utf8(const char *latin1, char *out, size_t cap);
void ameta_comment_to_latin1(const unsigned char *utf8, size_t len, char *out, size_t cap);

#endif
