/* j5n_symbols.c — [J5n] HUNK_SYMBOL parser (OURS, AROS-licensed). See j5n_symbols.h.
 * Independently re-walks the hunk file the j4 loader consumed and builds a PC->symbol
 * map; the loader itself is unchanged. No Emu68 source. */
#include "j5n_symbols.h"
#include <string.h>

/* Hunk type constants (mirror j4_hunk.h; we re-walk independently of the loader). */
#define HUNK_HEADER   1011u
#define HUNK_CODE     1001u
#define HUNK_DATA     1002u
#define HUNK_BSS      1003u
#define HUNK_RELOC32  1004u
#define HUNK_SYMBOL   1008u
#define HUNK_DEBUG    1009u
#define HUNK_END      1010u

typedef struct { const uint8_t *buf; size_t len, pos; } cur;

static int rd32(cur *c, uint32_t *out)
{
    if (c->pos + 4u > c->len) return 1;
    const uint8_t *p = c->buf + c->pos;
    *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    c->pos += 4u;
    return 0;
}

int j5n_symbols_parse(const uint8_t *buf, size_t len, const j4_seglist *seg,
                      j5n_symtab *out)
{
    memset(out, 0, sizeof(*out));
    cur c = { buf, len, 0 };
    uint32_t v;

    /* ---- HUNK_HEADER: skip exactly as the loader does, to reach the hunk body. ---- */
    if (rd32(&c, &v) || v != HUNK_HEADER) return 0;     /* not a hunk file: no symbols */
    for (;;) { if (rd32(&c, &v)) return 0; if (v == 0) break; c.pos += (size_t)v * 4u; }
    uint32_t numhunks, first, last;
    if (rd32(&c, &numhunks) || rd32(&c, &first) || rd32(&c, &last)) return 0;
    for (uint32_t i = 0; i < numhunks; i++) { if (rd32(&c, &v)) return 0; }

    /* ---- Walk the body. Track `lasthunk`, the CODE/DATA/BSS hunk a following SYMBOL
     * hunk attaches to (the symbol values are relative to that hunk's base). ---- */
    uint32_t curhunk = first, lasthunk = first;

    for (;;) {
        uint32_t htype;
        if (rd32(&c, &htype)) break;             /* EOF — done */
        htype &= 0xFFFFFFu;

        if (htype == HUNK_CODE || htype == HUNK_DATA || htype == HUNK_BSS) {
            if (rd32(&c, &v)) break;             /* length in longs */
            uint32_t bytes = v * 4u;
            if (htype != HUNK_BSS) c.pos += bytes;   /* skip the payload */
            lasthunk = curhunk;
            curhunk++;
            continue;
        }
        if (htype == HUNK_RELOC32) {
            for (;;) {
                uint32_t count; if (rd32(&c, &count)) return 0;
                if (count == 0) break;
                if (rd32(&c, &v)) return 0;       /* target hunk */
                c.pos += (size_t)count * 4u;      /* the offsets */
            }
            continue;
        }
        if (htype == HUNK_SYMBOL) {
            out->had_symbol_hunk = 1;
            uint32_t base = (lasthunk < (uint32_t)seg->numhunks)
                          ? seg->hunk_base[lasthunk] : 0u;
            for (;;) {
                uint32_t nlongs;
                if (rd32(&c, &nlongs)) return 0;
                if (nlongs == 0) break;          /* end of this SYMBOL hunk */
                /* name = nlongs longwords (NUL-padded), then a BE32 value. */
                size_t namebytes = (size_t)nlongs * 4u;
                if (c.pos + namebytes + 4u > c.len) return 0;
                const char *nm = (const char *)(c.buf + c.pos);
                c.pos += namebytes;
                uint32_t value;
                if (rd32(&c, &value)) return 0;

                if (out->n < J5N_MAX_SYMS) {
                    j5n_sym *s = &out->sym[out->n++];
                    size_t cpy = namebytes < (J5N_SYM_NAMELEN - 1)
                               ? namebytes : (J5N_SYM_NAMELEN - 1);
                    memcpy(s->name, nm, cpy);
                    s->name[cpy] = '\0';
                    /* trim trailing NUL padding already handled by the explicit '\0' */
                    s->addr = base + value;       /* hunk-relative -> runtime address */
                    s->hunk = lasthunk;
                }
            }
            continue;
        }
        if (htype == HUNK_DEBUG) {
            out->had_debug_hunk = 1;
            uint32_t longs; if (rd32(&c, &longs)) return 0;
            c.pos += (size_t)longs * 4u;          /* not parsed; reported only */
            continue;
        }
        if (htype == HUNK_END) {
            continue;                              /* one hunk ended; keep walking */
        }
        /* Unknown hunk type: we cannot safely skip it (no length), so stop. The symbols
         * collected so far are still valid. */
        break;
    }
    return 0;
}

const j5n_sym *j5n_symbols_lookup(const j5n_symtab *tab, uint32_t pc, uint32_t window,
                                  uint32_t *delta)
{
    if (!tab) return NULL;
    const j5n_sym *best = NULL;
    uint32_t best_addr = 0;
    for (int i = 0; i < tab->n; i++) {
        const j5n_sym *s = &tab->sym[i];
        if (s->addr <= pc && (best == NULL || s->addr > best_addr)) {
            best = s; best_addr = s->addr;
        }
    }
    if (best && window && (pc - best->addr) > window) best = NULL;
    if (best && delta) *delta = pc - best->addr;
    return best;
}
