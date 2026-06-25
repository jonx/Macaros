/* j4_loader.c — [J4] minimal hunk loader + HUNK_RELOC32 relocator (OURS).
 *
 * Clean-room / OURS. Implements a minimal big-endian AmigaOS hunk loader and the
 * HUNK_RELOC32 relocation, grounded against the REAL AROS loader
 *   rom/dos/internalloadseg_aos.c   (upstream tree)
 * and the hunk-type constants in
 *   compiler/include/dos/doshunks.h  (upstream tree).
 * No Emu68 source is used here — the loader/relocator/sandbox are entirely OURS.
 *
 * The relocation is reproduced byte-for-byte from internalloadseg_aos.c:292-332:
 * for each (target-hunk, offset) pair, read the BE32 value at the patch site, ADD
 * the runtime base of the target hunk, and write it back BE32. The only difference
 * from the real loader is WHERE the hunks live: the real loader AllocMem's each
 * hunk (MEMF_31BIT); we bump-allocate each inside a single 32-bit sandbox. The
 * relocation math (BE32 read, add target base, BE32 write) is identical.
 */
#include "j4_hunk.h"

#include <string.h>
#include <stdio.h>

/* ----- Big-endian helpers — the hunk file is 32-bit BIG-ENDIAN [PUB] ----------- */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* Read a BE32 from a sandbox host pointer (the relocator's *addr read). This is the
 * AROS_BE2LONG(*addr) step. */
static uint32_t sb_read_be32(const uint8_t *p) { return be32(p); }

/* Write a BE32 into the sandbox (the relocator's *addr = AROS_LONG2BE(val) step). */
static void sb_write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v      );
}

/* A tiny cursor over the hunk file with bounds checking. */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
} cur;

static int cur_u32(cur *c, uint32_t *out)
{
    if (c->pos + 4 > c->len) return 1;
    *out = be32(c->buf + c->pos);
    c->pos += 4;
    return 0;
}

/* ----- Sandbox bump allocator -------------------------------------------------- */
int j4_sandbox_init(j4_sandbox *sb, uint8_t *host_mem, uint32_t origin, uint32_t size)
{
    if (!sb || !host_mem) return 1;
    sb->host_mem       = host_mem;
    sb->sandbox_origin = origin;
    sb->size           = size;
    sb->next_alloc     = origin;
    memset(host_mem, 0, size);
    return 0;
}

/* Allocate `bytes` (16-byte aligned, like AROS MEMCHUNK_TOTAL rounding) inside the
 * sandbox; return the 68k-space (sandbox) address, or 0 on overflow. */
static uint32_t sandbox_alloc(j4_sandbox *sb, uint32_t bytes)
{
    uint32_t aligned = (bytes + 15u) & ~15u;
    uint32_t at = sb->next_alloc;
    /* end-of-sandbox check, in 68k-space */
    if ((uint64_t)at + aligned > (uint64_t)sb->sandbox_origin + sb->size)
        return 0;
    sb->next_alloc = at + aligned;
    return at;
}

/* ----- The loader -------------------------------------------------------------- */
int j4_load_hunks(j4_sandbox *sb, const uint8_t *buf, size_t len,
                  int skip_reloc, j4_seglist *seglist,
                  char *errbuf, unsigned errlen)
{
#define FAIL(msg) do { if (errbuf) snprintf(errbuf, errlen, "%s", (msg)); return 1; } while (0)

    cur c = { buf, len, 0 };
    uint32_t v;

    memset(seglist, 0, sizeof(*seglist));

    /* ---- HUNK_HEADER (1011) ----  internalloadseg_aos.c:126-222 --------------- */
    if (cur_u32(&c, &v)) FAIL("short file: no magic");
    if (v != J4_HUNK_HEADER) FAIL("bad magic (not HUNK_HEADER 1011)");

    /* Optional library-name strings: each is a BE32 length-in-longs (0 ends the
     * list). internalloadseg_aos.c:126-137. */
    for (;;) {
        if (cur_u32(&c, &v)) FAIL("short file in header name list");
        if (v == 0) break;               /* 0 => end of name list */
        c.pos += (size_t)v * 4u;         /* skip `v` longwords of name */
        if (c.pos > c.len) FAIL("header name list runs off end");
    }

    uint32_t numhunks, first, last;
    if (cur_u32(&c, &numhunks)) FAIL("short header: numhunks");   /* :138-141 */
    if (cur_u32(&c, &first))    FAIL("short header: first");      /* :151-154 */
    if (cur_u32(&c, &last))     FAIL("short header: last");       /* :158-161 */

    if (numhunks == 0 || numhunks > J4_MAX_HUNKS) FAIL("numhunks out of range");
    if (last < first || last >= numhunks)          FAIL("first/last out of range");

    seglist->numhunks = (int)numhunks;

    /* Per-hunk size words + sandbox allocation.  internalloadseg_aos.c:165-222.
     * lcount &= 0xFFFFFF; bytes = lcount*4. The top 2 bits are FAST/CHIP flags; we
     * mask them and reject the FAST|CHIP extra-MEMF-longword case (not in scope —
     * our test emits plain sizes). */
    for (uint32_t i = 0; i < numhunks; i++) {
        if (cur_u32(&c, &v)) FAIL("short header: hunk size word");
        uint32_t flags = v & 0xC0000000u;      /* HUNKF_FAST|HUNKF_CHIP top bits */
        if (flags == 0xC0000000u) FAIL("FAST|CHIP hunk flags not supported in spike");
        uint32_t longs = v & 0x00FFFFFFu;       /* :179 lcount &= 0xFFFFFF */
        uint32_t bytes = longs * 4u;            /* :180 bytes = lcount*4   */

        uint32_t base = sandbox_alloc(sb, bytes ? bytes : 16u);
        if (base == 0) FAIL("sandbox out of memory for hunk");
        seglist->hunk_base[i] = base;
        seglist->hunk_size[i] = bytes;
        seglist->hunk_type[i] = 0;              /* set when the payload arrives */
    }

    /* ---- Hunk body: CODE/DATA/BSS payloads, RELOC32 tables, END --------------- */
    uint32_t curhunk = first;     /* index of the next CODE/DATA/BSS hunk        */
    uint32_t lasthunk = first;    /* hunk whose payload was most recently loaded */
    int seen_code = 0;

    for (;;) {
        uint32_t htype;
        if (cur_u32(&c, &htype)) {
            /* End of stream. The real loader's main read loop (internalloadseg_aos.c:
             * 224 `while(!read_block_buffered(...))`) terminates when the file is
             * exhausted; that is the NORMAL way a hunk file ends once every payload
             * hunk has been read. So EOF here is success IFF all declared payload
             * hunks were loaded (curhunk > last); otherwise the file is truncated. */
            if (curhunk > last) break;
            FAIL("unexpected end of hunk stream (missing payload hunks)");
        }
        htype &= 0xFFFFFFu;       /* internalloadseg_aos.c:229 mask off flag bits */

        if (htype == J4_HUNK_CODE || htype == J4_HUNK_DATA || htype == J4_HUNK_BSS) {
            /* internalloadseg_aos.c:269-290 */
            if (cur_u32(&c, &v)) FAIL("short CODE/DATA/BSS length");
            uint32_t longs = v;
            uint32_t bytes = longs * 4u;
            if (curhunk >= numhunks) FAIL("more payload hunks than header declared");

            if (bytes != seglist->hunk_size[curhunk])
                FAIL("payload size disagrees with header size word");

            seglist->hunk_type[curhunk] = htype;

            if (htype != J4_HUNK_BSS && bytes) {
                /* Copy the payload into the sandbox at this hunk's runtime base. */
                if (c.pos + bytes > c.len) FAIL("CODE/DATA payload runs off end");
                uint8_t *dst = j4_sandbox_host(sb, seglist->hunk_base[curhunk]);
                memcpy(dst, c.buf + c.pos, bytes);
                c.pos += bytes;
            }
            /* BSS: already zeroed by sandbox_init; no file bytes consumed. */

            if (htype == J4_HUNK_CODE && !seen_code) {
                seglist->entry = seglist->hunk_base[curhunk];  /* first CODE hunk */
                seen_code = 1;
            }

            lasthunk = curhunk;       /* :288 lasthunk = curhunk */
            curhunk++;                /* :289 ++curhunk          */
            continue;
        }

        if (htype == J4_HUNK_RELOC32) {
            /* internalloadseg_aos.c:292-332 — the exact big-endian algorithm. */
            for (;;) {
                uint32_t count;
                if (cur_u32(&c, &count)) FAIL("short RELOC32 count");
                if (count == 0) break;                     /* :301 end of table */

                uint32_t target;
                if (cur_u32(&c, &target)) FAIL("short RELOC32 target-hunk");
                if (target >= numhunks) FAIL("RELOC32 target hunk out of range");

                for (uint32_t k = 0; k < count; k++) {
                    uint32_t offset;
                    if (cur_u32(&c, &offset)) FAIL("short RELOC32 offset");

                    if (skip_reloc)
                        continue;          /* NEGATIVE CONTROL: leave the pointer raw */

                    /* addr = GETHUNKPTR(lasthunk) + offset   (:321)
                     * The patch site lives in `lasthunk` (the most recently loaded
                     * CODE/DATA hunk), at byte `offset`. */
                    uint32_t site_addr = seglist->hunk_base[lasthunk] + offset;
                    if (offset + 4u > seglist->hunk_size[lasthunk])
                        FAIL("RELOC32 offset outside lasthunk");
                    uint8_t *site = j4_sandbox_host(sb, site_addr);

                    /* val = AROS_BE2LONG(*addr) + (IPTR)GETHUNKPTR(target)   (:323)
                     * The runtime base added is the TARGET hunk's payload base. In
                     * our sandbox that is the 32-bit sandbox runtime address. */
                    uint32_t cur_val = sb_read_be32(site);
                    uint32_t reloc   = cur_val + seglist->hunk_base[target];

                    /* *addr = AROS_LONG2BE(val)   (:326) — write back big-endian. */
                    sb_write_be32(site, reloc);
                }
            }
            continue;
        }

        if (htype == J4_HUNK_SYMBOL) {
            /* internalloadseg_aos.c:231-252 — SKIP the debug symbol table. The
             * SYMBOL hunk is a repeated group: a BE32 name-length-in-longs `n`
             * (0 ends the hunk), then `n` longwords of name + 1 longword value.
             * The real loader seek_forwards (lcount+1) longs per symbol; we
             * advance the cursor identically. Real Amiga executables are usually
             * stripped (vasm -nosym), but tolerating symbols here matches the real
             * loader and lets un-stripped hunk files load. */
            for (;;) {
                uint32_t n;
                if (cur_u32(&c, &n)) FAIL("short HUNK_SYMBOL name length");
                if (n == 0) break;                  /* 0 => end of symbol hunk */
                c.pos += ((size_t)n + 1u) * 4u;     /* n name longs + 1 value long */
                if (c.pos > c.len) FAIL("HUNK_SYMBOL runs off end");
            }
            continue;
        }

        if (htype == J4_HUNK_DEBUG) {
            /* internalloadseg_aos.c:478-488 — SKIP the debug-info hunk: a BE32
             * length-in-longs, then that many longwords, all ignored. */
            uint32_t longs;
            if (cur_u32(&c, &longs)) FAIL("short HUNK_DEBUG length");
            c.pos += (size_t)longs * 4u;
            if (c.pos > c.len) FAIL("HUNK_DEBUG runs off end");
            continue;
        }

        if (htype == J4_HUNK_END) {
            /* internalloadseg_aos.c:450-460. HUNK_END terminates ONE hunk, NOT the
             * whole program: the real loader merely `break`s the switch and the main
             * while-loop reads the next hunk. (A multi-hunk file such as CODE +
             * RELOC32 + END + DATA + END has an END after EACH hunk.) We stop only
             * once every declared payload hunk has been loaded (curhunk > last),
             * mirroring the real loader's `curhunk > last` exit test; otherwise we
             * continue to the next hunk. */
            if (curhunk > last) break;
            continue;
        }

        FAIL("unsupported hunk type in spike loader");
    }

    if (!seen_code) FAIL("no HUNK_CODE found (no entry)");
    return 0;
#undef FAIL
}
