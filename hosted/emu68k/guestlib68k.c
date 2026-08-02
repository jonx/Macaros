/* [T3e] Resident/MakeLibrary construction that stays entirely in guest memory.
 * Grounded against AROS rom/exec/{initresident,makelibrary,makefunctions,
 * initstruct}.c and the classic m68k six-byte JumpVec ABI. */

#include "guestlib68k.h"

#include <stdio.h>
#include <string.h>

#define RTC_MATCHWORD 0x4afcu
#define NT_LIBRARY 9u
#define LIBF_CHANGED 0x02u
#define LIBF_SUMUSED 0x04u
#define GL68_MAX_VECTORS 2048u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int segment_span(const j4_seglist *seg, uint32_t addr, uint32_t bytes,
                        int code_only)
{
    uint64_t end = (uint64_t)addr + bytes;
    for (int i = 0; i < seg->numhunks; i++) {
        uint64_t lo = seg->hunk_base[i];
        uint64_t hi = lo + seg->hunk_size[i];
        if (code_only && seg->hunk_type[i] != J4_HUNK_CODE) continue;
        if ((uint64_t)addr >= lo && end <= hi) return 1;
    }
    return 0;
}

static int segment_point_or_end(const j4_seglist *seg, uint32_t addr)
{
    for (int i = 0; i < seg->numhunks; i++) {
        uint64_t lo = seg->hunk_base[i];
        uint64_t hi = lo + seg->hunk_size[i];
        if ((uint64_t)addr >= lo && (uint64_t)addr <= hi) return 1;
    }
    return 0;
}

static int guest_string(const j4_sandbox *sb, const j4_seglist *seg,
                        uint32_t addr, char *dst, unsigned dstlen)
{
    if (!dstlen) return 0;
    for (int i = 0; i < seg->numhunks; i++) {
        uint32_t lo = seg->hunk_base[i], size = seg->hunk_size[i];
        if (addr < lo || addr >= lo + size) continue;
        uint32_t avail = lo + size - addr;
        const uint8_t *p = j4_sandbox_host(sb, addr);
        uint32_t n;
        for (n = 0; n < avail && p[n]; n++) {
            if (n + 1 >= dstlen) return 0;
            dst[n] = (char)p[n];
        }
        if (n == avail) return 0;
        dst[n] = 0;
        return 1;
    }
    return 0;
}

int gl68_find_resident(const j4_sandbox *sb, const j4_seglist *seg,
                       const char *requested_name, gl68_resident *out,
                       char *err, unsigned errlen)
{
    if (!sb || !seg || !requested_name || !out) {
        if (err) snprintf(err, errlen, "invalid resident lookup arguments");
        return 1;
    }
    for (int h = 0; h < seg->numhunks; h++) {
        if (seg->hunk_type[h] != J4_HUNK_CODE &&
            seg->hunk_type[h] != J4_HUNK_DATA) continue;
        uint32_t base = seg->hunk_base[h], size = seg->hunk_size[h];
        const uint8_t *p = j4_sandbox_host(sb, base);
        for (uint32_t off = 0; off + 26u <= size; off += 2u) {
            gl68_resident r;
            uint32_t tag = base + off;
            if (rd16(p + off) != RTC_MATCHWORD || rd32(p + off + 2) != tag)
                continue;
            memset(&r, 0, sizeof r);
            r.tag = tag;
            r.end_skip = rd32(p + off + 6);
            r.flags = p[off + 10]; r.version = p[off + 11];
            r.type = p[off + 12]; r.priority = (int8_t)p[off + 13];
            r.name_ptr = rd32(p + off + 14);
            r.id_ptr = rd32(p + off + 18);
            r.init = rd32(p + off + 22);
            if (r.type != NT_LIBRARY) continue;
            if (!segment_point_or_end(seg, r.end_skip) || r.end_skip < tag + 26u)
                continue;
            if (!guest_string(sb, seg, r.name_ptr, r.name, sizeof r.name))
                continue;
            if (strcmp(r.name, requested_name)) continue;
            { char id[256]; if (!guest_string(sb, seg, r.id_ptr, id, sizeof id)) {
                    if (err) snprintf(err, errlen, "%s has an invalid rt_IdString", requested_name);
                    return 1;
                } }
            if (!r.init || !segment_span(seg, r.init,
                    (r.flags & GL68_RTF_AUTOINIT) ? 16u : 2u,
                    (r.flags & GL68_RTF_AUTOINIT) ? 0 : 1)) {
                if (err) snprintf(err, errlen, "%s has an invalid rt_Init", requested_name);
                return 1;
            }
            if ((r.flags & GL68_RTF_EXTENDED) && segment_span(seg, tag, 28, 0))
                r.revision = rd16(j4_sandbox_host(sb, tag + 26));
            *out = r;
            return 0;
        }
    }
    if (err) snprintf(err, errlen, "NT_LIBRARY resident %s not found", requested_name);
    return 1;
}

static uint32_t alloc_zero(j4_sandbox *sb, uint32_t bytes)
{
    uint32_t at = (sb->next_alloc + 1u) & ~1u;
    uint32_t n = (bytes + 1u) & ~1u;
    if (!n || (uint64_t)at + n > (uint64_t)sb->sandbox_origin + sb->size)
        return 0;
    sb->next_alloc = at + n;
    memset(j4_sandbox_host(sb, at), 0, n);
    return at;
}

static int apply_initstruct(j4_sandbox *sb, const j4_seglist *seg,
                            uint32_t table, uint32_t base, uint32_t size,
                            char *err, unsigned errlen)
{
    uint32_t ip = table, dst = base, steps = 0;
    while (steps++ < 4096u) {
        uint8_t op, action, width_code;
        uint32_t count, width, offset = 0, src;
        if (!segment_span(seg, ip, 1, 0)) goto bad_stream;
        op = *j4_sandbox_host(sb, ip);
        if (!op) return 0;
        action = (op >> 6) & 3u; width_code = (op >> 4) & 3u;
        count = (op & 15u) + 1u;
        width = width_code == 0 ? 4u : width_code == 1 ? 2u :
                width_code == 2 ? 1u : 8u;
        if (action <= 1u) ip++;
        else if (action == 2u) {
            if (!segment_span(seg, ip, 2, 0)) goto bad_stream;
            offset = j4_sandbox_host(sb, ip)[1]; ip += 2u;
        } else {
            if (!segment_span(seg, ip, 4, 0)) goto bad_stream;
            offset = rd32(j4_sandbox_host(sb, ip)) & 0x00ffffffu; ip += 4u;
        }
        ip = (ip + 1u) & ~1u; dst = (dst + 1u) & ~1u;
        if (action >= 2u) dst = base + offset;
        src = ip;
        if ((uint64_t)dst + (uint64_t)count * width > (uint64_t)base + size) {
            if (err) snprintf(err, errlen, "InitStruct writes outside positive library area");
            return 1;
        }
        if (action == 1u) {
            if (!segment_span(seg, src, width, 0)) goto bad_stream;
            for (uint32_t i = 0; i < count; i++)
                memcpy(j4_sandbox_host(sb, dst + i * width),
                       j4_sandbox_host(sb, src), width);
            ip += width;
        } else {
            uint32_t bytes = count * width;
            if (!segment_span(seg, src, bytes, 0)) goto bad_stream;
            memcpy(j4_sandbox_host(sb, dst), j4_sandbox_host(sb, src), bytes);
            ip += bytes;
        }
        dst += count * width;
        ip = (ip + 1u) & ~1u;
    }
    if (err) snprintf(err, errlen, "InitStruct instruction limit exceeded");
    return 1;
bad_stream:
    if (err) snprintf(err, errlen, "InitStruct stream leaves loaded segments");
    return 1;
}

int gl68_prepare_init(j4_sandbox *sb, const j4_seglist *seg,
                      const gl68_resident *r, gl68_init *out,
                      char *err, unsigned errlen)
{
    uint32_t table, dsize, functions, structure, initfn, nvec = 0;
    uint32_t old_next;
    int relative;
    if (!sb || !seg || !r || !out) {
        if (err) snprintf(err, errlen, "invalid library-init arguments");
        return 1;
    }
    memset(out, 0, sizeof *out);
    out->seglist = seg->entry;
    out->init_pc = r->init;
    if (!(r->flags & GL68_RTF_AUTOINIT)) return 0;

    table = r->init;
    dsize = rd32(j4_sandbox_host(sb, table));
    functions = rd32(j4_sandbox_host(sb, table + 4));
    structure = rd32(j4_sandbox_host(sb, table + 8));
    initfn = rd32(j4_sandbox_host(sb, table + 12));
    if (dsize < 34u || dsize > 0xffffu) {
        if (err) snprintf(err, errlen, "AUTOINIT positive size %u is invalid", dsize);
        return 1;
    }
    if (!segment_span(seg, functions, 2, 0) ||
        (initfn && !segment_span(seg, initfn, 2, 1)) ||
        (structure && !segment_span(seg, structure, 1, 0))) {
        if (err) snprintf(err, errlen, "AUTOINIT table contains an out-of-image pointer");
        return 1;
    }
    relative = rd16(j4_sandbox_host(sb, functions)) == 0xffffu;
    if (relative) {
        uint32_t fp = functions + 2u;
        while (nvec < GL68_MAX_VECTORS) {
            if (!segment_span(seg, fp, 2, 0)) break;
            if ((int16_t)rd16(j4_sandbox_host(sb, fp)) == -1) break;
            nvec++; fp += 2u;
        }
        if (!segment_span(seg, fp, 2, 0)) nvec = GL68_MAX_VECTORS;
    } else {
        uint32_t fp = functions;
        while (nvec < GL68_MAX_VECTORS) {
            if (!segment_span(seg, fp, 4, 0)) break;
            if (rd32(j4_sandbox_host(sb, fp)) == 0xffffffffu) break;
            nvec++; fp += 4u;
        }
        if (!segment_span(seg, fp, 4, 0)) nvec = GL68_MAX_VECTORS;
    }
    if (!nvec || nvec == GL68_MAX_VECTORS) {
        if (err) snprintf(err, errlen, "AUTOINIT function table is unterminated or empty");
        return 1;
    }

    /* Validate the complete table before allocating, so a malformed late
     * entry cannot leave a half-constructed library in the guest heap. */
    for (uint32_t i = 0; i < nvec; i++) {
        uint32_t fn;
        if (relative) {
            int16_t disp = (int16_t)rd16(j4_sandbox_host(sb, functions + 2u + i * 2u));
            if (!disp) { if (err) snprintf(err, errlen, "zero relative library vector unsupported"); return 1; }
            fn = functions + (uint32_t)(int32_t)disp;
        } else {
            fn = rd32(j4_sandbox_host(sb, functions + i * 4u));
            if (!fn) { if (err) snprintf(err, errlen, "zero absolute library vector unsupported"); return 1; }
        }
        if (!segment_span(seg, fn, 2, 1)) {
            if (err) snprintf(err, errlen, "library vector %u points outside CODE hunks", i + 1u);
            return 1;
        }
    }

    out->neg_size = nvec * 6u; out->pos_size = dsize; out->vectors = nvec;
    old_next = sb->next_alloc;
    {
        uint32_t alloc = alloc_zero(sb, out->neg_size + dsize);
        if (!alloc) { if (err) snprintf(err, errlen, "no guest memory for library"); return 1; }
        out->base = alloc + out->neg_size;
    }
    for (uint32_t i = 0; i < nvec; i++) {
        uint32_t fn;
        if (relative) {
            int16_t disp = (int16_t)rd16(j4_sandbox_host(sb, functions + 2u + i * 2u));
            fn = functions + (uint32_t)(int32_t)disp;
        } else {
            fn = rd32(j4_sandbox_host(sb, functions + i * 4u));
        }
        uint8_t *v = j4_sandbox_host(sb, out->base - (i + 1u) * 6u);
        wr16(v, 0x4ef9u); wr32(v + 2, fn);
    }
    if (structure && apply_initstruct(sb, seg, structure, out->base, dsize, err, errlen)) {
        memset(j4_sandbox_host(sb, (old_next + 1u) & ~1u), 0,
               sb->next_alloc - ((old_next + 1u) & ~1u));
        sb->next_alloc = old_next;
        memset(out, 0, sizeof *out);
        return 1;
    }

    /* Classic 68k struct Library fields copied by InitResident after MakeLibrary. */
    uint8_t *lib = j4_sandbox_host(sb, out->base);
    lib[8] = r->type; wr32(lib + 10, r->name_ptr);
    lib[14] = LIBF_CHANGED | LIBF_SUMUSED;
    wr16(lib + 16, (uint16_t)out->neg_size);
    wr16(lib + 18, (uint16_t)dsize);
    wr16(lib + 20, r->version);
    wr16(lib + 22, r->revision);
    wr32(lib + 24, r->id_ptr);
    out->init_pc = initfn;
    return 0;
}
