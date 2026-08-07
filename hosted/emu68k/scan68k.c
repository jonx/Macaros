/* scan68k.c — [T2a] the static hardware-use scanner (OURS, AROS-licensed).
 * See scan68k.h for the contract and the honest limits. Contains NO Emu68
 * source: this walks the hunk file format and matches opcode encodings from
 * the published 68000 ISA; it does not decode or translate anything. */

#include "scan68k.h"
#include "emu68k_genlibs.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- hunk file constants (dos/doshunks.h) ---- */
#define HUNK_HEADER   1011u
#define HUNK_CODE     1001u
#define HUNK_DATA     1002u
#define HUNK_BSS      1003u
#define HUNK_RELOC32  1004u
#define HUNK_SYMBOL   1008u
#define HUNK_DEBUG    1009u
#define HUNK_END      1010u

/* ---- the hardware ranges a translated engine cannot serve ---- */
#define CUSTOM_LO  0x00DFF000ul
#define CUSTOM_HI  0x00DFFFFFul
#define CIA_LO     0x00BFD000ul
#define CIA_HI     0x00BFEFFFul
#define VECTOR_HI  0x000003FFul     /* exception vector page                    */

static uint32_t be32at(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t be16at(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void add_ev(scan68k_report *r, scan68k_evkind kind, int hunk,
                   unsigned long off, unsigned long value, int in_context,
                   const char *what)
{
    if (r->n_evidence >= SCAN68K_MAX_EVIDENCE) { r->truncated = 1; return; }
    scan68k_evidence *e = &r->evidence[r->n_evidence++];
    e->kind = kind; e->hunk = hunk; e->offset = off;
    e->value = value; e->in_context = in_context;
    snprintf(e->what, sizeof e->what, "%s", what);
}

/* Is this instruction word a supervisor operation the ENGINE cannot serve?
 * Encodings from the M68000 PRM. The list is deliberately about our engine's
 * actual reach, not about privilege in the abstract: RTE is privileged but the
 * engine implements 68k exception dispatch and return, so a program with an
 * exception handler runs fine and must not be routed away. What is listed here
 * is machine control with no API meaning under translation. */
static const char *privileged_word(uint16_t w)
{
    if (w == 0x4E70u) return "RESET";
    if (w == 0x4E72u) return "STOP #imm";
    /* ORI/ANDI/EORI to SR are NOT here. They are privileged on the real
     * machine, but the question this list answers is what the ENGINE can
     * serve, and the bits a program sets in the SR are the interrupt mask -
     * which has no meaning under translation - plus the condition codes, which
     * the engine owns. Vetoing them statically routed a whole class of
     * ordinary applications away over one instruction in their startup code,
     * and if one ever does something the engine cannot serve, the runtime says
     * so at the instruction rather than the scanner guessing beforehand. */
    if ((w & 0xFFF0u) == 0x4E60u) return "MOVE USP";
    if (w == 0x4E7Au || w == 0x4E7Bu) return "MOVEC";
    /* MOVE to SR: 0100 0110 11 mmmrrr — any source EA */
    if ((w & 0xFFC0u) == 0x46C0u) return "MOVE to SR";
    return NULL;
}

/* ---- instruction lengths, so the scan reads OPCODES and not OPERANDS -------
 * A pure 2-byte sweep interprets immediate values and addresses as if they were
 * instructions, which fabricates "privileged instruction" and "vector store"
 * findings all over ordinary compiled code (it did: every corpus program came
 * back a hardware banger). Walking real instruction boundaries removes that
 * whole class. The walk stops at the first word it cannot size - it never
 * guesses a resync, because a desynced walk is worse than a short one; the
 * whole-hunk value scan still covers the rest as weak evidence.
 *
 * Sizes are from the published M68000 encoding: base word plus the extension
 * words each effective address carries. */

/* Extension bytes an effective address adds. sz: 0=byte 1=word 2=long.
 * Returns -1 for an encoding we do not size. */
static int ea_ext(unsigned mode, unsigned reg, unsigned sz)
{
    switch (mode) {
    case 0: case 1: case 2: case 3: case 4: return 0;   /* Dn An (An) (An)+ -(An) */
    case 5: return 2;                                    /* (d16,An)               */
    case 6: return 2;                                    /* (d8,An,Xn) brief       */
    case 7:
        switch (reg) {
        case 0: return 2;                                /* abs.w                  */
        case 1: return 4;                                /* abs.l                  */
        case 2: return 2;                                /* (d16,PC)               */
        case 3: return 2;                                /* (d8,PC,Xn)             */
        case 4: return (sz == 2) ? 4 : 2;                /* immediate              */
        default: return -1;
        }
    default: return -1;
    }
}

/* Length in bytes of the instruction at `w` (with `next` = the following word,
 * needed for Bcc), or 0 when unknown. */
static int insn_len(uint16_t w, uint16_t next)
{
    unsigned line = (w >> 12) & 0xFu;
    unsigned mode = (w >> 3) & 0x7u, reg = w & 0x7u;
    int ext;

    switch (line) {
    case 0x0: {                                    /* immediate / bit ops        */
        if ((w & 0xFFu) == 0x7Cu) return 4;        /* ORI/ANDI/EORI to SR        */
        if ((w & 0xFFu) == 0x3Cu) return 4;        /* ...to CCR                  */
        if (w & 0x0100u) {                         /* BTST/BCHG/BCLR/BSET Dn     */
            ext = ea_ext(mode, reg, 0); return ext < 0 ? 0 : 2 + ext;
        }
        if ((w & 0x0F00u) == 0x0800u) {            /* static bit op: +2 immediate */
            ext = ea_ext(mode, reg, 0); return ext < 0 ? 0 : 4 + ext;
        }
        {   unsigned sz = (w >> 6) & 3u;
            if (sz > 2) return 0;
            ext = ea_ext(mode, reg, sz);
            if (ext < 0) return 0;
            return 2 + (sz == 2 ? 4 : 2) + ext;    /* immediate then EA          */
        }
    }
    case 0x1: case 0x2: case 0x3: {                /* MOVE / MOVEA               */
        unsigned sz = (line == 0x1) ? 0 : (line == 0x3) ? 1 : 2;
        unsigned dmode = (w >> 6) & 0x7u, dreg = (w >> 9) & 0x7u;
        int se = ea_ext(mode, reg, sz), de = ea_ext(dmode, dreg, sz);
        if (se < 0 || de < 0) return 0;
        return 2 + se + de;
    }
    case 0x4: {                                    /* misc                       */
        if (w == 0x4E70u || w == 0x4E71u || w == 0x4E73u ||
            w == 0x4E75u || w == 0x4E76u || w == 0x4E77u) return 2;
        if (w == 0x4E72u) return 4;                /* STOP #imm                  */
        if ((w & 0xFFF0u) == 0x4E40u) return 2;    /* TRAP #n                    */
        if ((w & 0xFFF8u) == 0x4E50u) return 4;    /* LINK.W                     */
        if ((w & 0xFFF8u) == 0x4E58u) return 2;    /* UNLK                       */
        if ((w & 0xFFF0u) == 0x4E60u) return 2;    /* MOVE USP                   */
        if (w == 0x4E7Au || w == 0x4E7Bu) return 4;/* MOVEC                      */
        if ((w & 0xFFC0u) == 0x4E80u ||            /* JSR                        */
            (w & 0xFFC0u) == 0x4EC0u) {            /* JMP                        */
            ext = ea_ext(mode, reg, 2); return ext < 0 ? 0 : 2 + ext;
        }
        if ((w & 0xF1C0u) == 0x41C0u) {            /* LEA                        */
            ext = ea_ext(mode, reg, 2); return ext < 0 ? 0 : 2 + ext;
        }
        if ((w & 0xFFC0u) == 0x4840u && mode >= 2) {/* PEA                       */
            ext = ea_ext(mode, reg, 2); return ext < 0 ? 0 : 2 + ext;
        }
        if ((w & 0xFB80u) == 0x4880u && mode >= 2) {/* MOVEM: +2 mask + EA       */
            ext = ea_ext(mode, reg, 2); return ext < 0 ? 0 : 4 + ext;
        }
        if ((w & 0xFFB8u) == 0x4880u) return 2;    /* EXT.W / EXT.L              */
        if (w == 0x4AFCu) return 2;                /* ILLEGAL                    */
        if ((w & 0xFFC0u) == 0x40C0u ||            /* MOVE from SR               */
            (w & 0xFFC0u) == 0x44C0u ||            /* MOVE to CCR                */
            (w & 0xFFC0u) == 0x46C0u) {            /* MOVE to SR                 */
            ext = ea_ext(mode, reg, 1); return ext < 0 ? 0 : 2 + ext;
        }
        {   unsigned sz = (w >> 6) & 3u;           /* CLR/NEG/NOT/TST/NBCD/...   */
            if (sz > 2) return 0;
            ext = ea_ext(mode, reg, sz); return ext < 0 ? 0 : 2 + ext;
        }
    }
    case 0x5: {                                    /* ADDQ/SUBQ/Scc/DBcc         */
        if ((w & 0xF0F8u) == 0x50C8u) return 4;    /* DBcc                       */
        if ((w & 0xF0C0u) == 0x50C0u) {            /* Scc                        */
            ext = ea_ext(mode, reg, 0); return ext < 0 ? 0 : 2 + ext;
        }
        {   unsigned sz = (w >> 6) & 3u;
            if (sz > 2) return 0;
            ext = ea_ext(mode, reg, sz); return ext < 0 ? 0 : 2 + ext;
        }
    }
    case 0x6: {                                    /* Bcc / BRA / BSR            */
        unsigned d8 = w & 0xFFu;
        (void)next;
        if (d8 == 0x00u) return 4;                 /* .W displacement            */
        if (d8 == 0xFFu) return 6;                 /* .L displacement (68020)    */
        return 2;
    }
    case 0x7: return (w & 0x0100u) ? 0 : 2;        /* MOVEQ                      */
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        unsigned opmode = (w >> 6) & 0x7u;
        unsigned sz = (opmode == 3u || opmode == 7u) ? 1u : (opmode & 3u);
        if (sz > 2) return 0;
        ext = ea_ext(mode, reg, sz); return ext < 0 ? 0 : 2 + ext;
    }
    case 0xE: {                                    /* shifts / rotates           */
        if (((w >> 6) & 3u) == 3u) {               /* memory shift: one EA, word */
            ext = ea_ext(mode, reg, 1); return ext < 0 ? 0 : 2 + ext;
        }
        return 2;                                  /* register shift             */
    }
    default: return 0;                             /* line A/F: stop the walk    */
    }
}

static int classify_addr(unsigned long a, scan68k_evkind *kind, const char **name)
{
    if (a >= CUSTOM_LO && a <= CUSTOM_HI) { *kind = SCAN68K_EV_CUSTOM; *name = "custom chip"; return 1; }
    if (a >= CIA_LO    && a <= CIA_HI)    { *kind = SCAN68K_EV_CIA;    *name = "CIA";         return 1; }
    return 0;
}

/* Where inside this instruction does its (single) EA extension start? */
static unsigned ea_ext_offset(uint16_t w)
{
    unsigned line = (w >> 12) & 0xFu;
    if (line == 0x0u) {                      /* immediate op: immediate first  */
        if (w & 0x0100u) return 2;           /* dynamic bit op: no immediate   */
        if ((w & 0x0F00u) == 0x0800u) return 4;  /* static bit op: +2          */
        return 2 + ((((w >> 6) & 3u) == 2u) ? 4u : 2u);
    }
    if (line == 0x4u && (w & 0xFB80u) == 0x4880u && ((w >> 3) & 7u) >= 2u)
        return 4;                            /* MOVEM: after the register mask */
    return 2;
}

/* Record any absolute hardware/vector address this instruction addresses. With
 * real instruction lengths the extension layout is known exactly, so this reads
 * the actual operand instead of guessing which word it is. */
/* Relocation sites of the CODE hunk being scanned: longword offsets the loader
 * patches with a runtime address. An "absolute address" sitting at one of these
 * is a pointer to the program's own data, not a hardware register - dhrystone
 * initialising its globals with `move.l #0,$64` is not touching an exception
 * vector. Excluding them removes the last false-positive class the boundary
 * walk cannot see. */
#define SCAN68K_RELOC_BITS  8192              /* 1 bit per 2 bytes: 128 KiB code */
struct reloc_set { unsigned char bits[SCAN68K_RELOC_BITS]; };

static void reloc_mark(struct reloc_set *rs, unsigned long off)
{
    unsigned long idx = off >> 1;
    if (idx < SCAN68K_RELOC_BITS * 8u) rs->bits[idx >> 3] |= (unsigned char)(1u << (idx & 7u));
}
static int is_reloc_site(const struct reloc_set *rs, unsigned long off)
{
    unsigned long idx;
    if (!rs || (off & 1u)) return 0;
    idx = off >> 1;
    if (idx >= SCAN68K_RELOC_BITS * 8u) return 0;
    return (rs->bits[idx >> 3] >> (idx & 7u)) & 1u;
}

static void check_abs_operand(const uint8_t *code, unsigned long len,
                              unsigned long off, uint16_t w, int hunk,
                              const struct reloc_set *rs, scan68k_report *r)
{
    unsigned line = (w >> 12) & 0xFu;
    char buf[64];
    struct { unsigned mode, reg, at; } slot[2];
    int nslot = 0;

    if (line == 0x1u || line == 0x2u || line == 0x3u) {      /* MOVE / MOVEA   */
        unsigned sz = (line == 0x1u) ? 0u : (line == 0x3u) ? 1u : 2u;
        unsigned smode = (w >> 3) & 7u, sreg = w & 7u;
        unsigned dmode = (w >> 6) & 7u, dreg = (w >> 9) & 7u;
        int se = ea_ext(smode, sreg, sz);
        if (se < 0) return;
        slot[nslot].mode = smode; slot[nslot].reg = sreg; slot[nslot].at = 2;
        nslot++;
        slot[nslot].mode = dmode; slot[nslot].reg = dreg;
        slot[nslot].at = 2u + (unsigned)se;
        nslot++;
    } else {
        slot[0].mode = (w >> 3) & 7u; slot[0].reg = w & 7u;
        slot[0].at = ea_ext_offset(w);
        nslot = 1;
    }

    for (int s = 0; s < nslot; s++) {
        unsigned long a;
        if (slot[s].mode != 7u) continue;
        if (slot[s].reg == 1u) {                                   /* abs.l    */
            if (off + slot[s].at + 4 > len) continue;
            if (is_reloc_site(rs, off + slot[s].at)) continue;  /* own data    */
            a = be32at(code + off + slot[s].at);
        } else if (slot[s].reg == 0u) {                            /* abs.w    */
            if (off + slot[s].at + 2 > len) continue;
            a = (unsigned long)(uint32_t)(int32_t)(int16_t)be16at(code + off + slot[s].at);
        } else continue;

        scan68k_evkind k; const char *nm;
        if (classify_addr(a, &k, &nm)) {
            snprintf(buf, sizeof buf, "absolute access to $%06lX (%s)", a, nm);
            add_ev(r, k, hunk, off, a, 1, buf);
        } else if (a >= 8 && a <= VECTOR_HI && (a & 3u) == 0 &&
                   s == nslot - 1 && nslot == 2) {
            /* a MOVE whose DESTINATION is a longword-aligned exception vector */
            snprintf(buf, sizeof buf, "store into exception vector $%03lX", a);
            add_ev(r, SCAN68K_EV_VECTOR, hunk, off, a, 1, buf);
        }
    }
}

/* Scan one CODE hunk. */
static void scan_code(const uint8_t *code, unsigned long len, int hunk,
                      const struct reloc_set *rs, scan68k_report *r)
{
    unsigned long i;

    /* (1) walk REAL instruction boundaries from the hunk entry. Findings here
     * are strong: the word really is an opcode and its operands are located
     * exactly. The walk ends at the first word it cannot size - it never
     * guesses a resync, because a desynced walk invents findings. */
    for (i = 0; i + 2 <= len; ) {
        uint16_t w = be16at(code + i);
        uint16_t next = (i + 4 <= len) ? be16at(code + i + 2) : 0;
        char buf[64];

        const char *priv = privileged_word(w);
        if (priv) {
            snprintf(buf, sizeof buf, "%s (privileged)", priv);
            add_ev(r, SCAN68K_EV_PRIVILEGED, hunk, i, w, 1, buf);
        } else if (!((w & 0xfff8u) == 0x33c0u && i + 6 <= len &&
                     be32at(code + i + 2) == 0x00dff180u)) {
            /* The runtime serves the exact `move.w Dn,$DFF180` desktop
             * calibration spelling as a flag-correct write sink. It is not a
             * reason to route the whole application away from the JIT. */
            check_abs_operand(code, len, i, w, hunk, rs, r);
        }

        /* [T3] a library-call site: jsr d16(a6) with a negative displacement.
         * (A6 as a frame pointer gives positive/zero displacements.) */
        if (w == 0x4EAEu && i + 4 <= len) {
            int16_t d16 = (int16_t)be16at(code + i + 2);
            if (d16 < 0) {
                r->n_lib_calls++;
                int seen = 0;
                for (int k = 0; k < r->n_lvo_off; k++)
                    if (r->lvo_off[k] == d16) { seen = 1; break; }
                if (!seen && r->n_lvo_off < SCAN68K_MAX_LVOOFF)
                    r->lvo_off[r->n_lvo_off++] = d16;
            }
        }

        int l = insn_len(w, next);
        if (l <= 0) break;
        i += (unsigned long)l;
    }
    r->walked_bytes += i;      /* how much of this hunk the walk actually read */

    /* (3) hardware-shaped LONGWORDS anywhere in the hunk, WITHOUT instruction
     * context: inline data, a jump table, a computed base. Recorded as weak
     * evidence only - this is exactly the false-positive class the confidence
     * grading exists for. Skipped where an in-context hit already covers it. */
    for (i = 0; i + 4 <= len; i += 2) {
        unsigned long a = be32at(code + i);
        scan68k_evkind k; const char *nm;
        if (is_reloc_site(rs, i)) continue;
        if (!classify_addr(a, &k, &nm)) continue;
        int covered = 0, j;
        for (j = 0; j < r->n_evidence; j++)
            if (r->evidence[j].in_context && r->evidence[j].hunk == hunk &&
                r->evidence[j].value == a) { covered = 1; break; }
        if (covered) continue;
        char buf[64];
        snprintf(buf, sizeof buf, "$%06lX (%s) as data, no instruction context", a, nm);
        add_ev(r, k, hunk, i, a, 0, buf);
    }
}

int scan68k_image(const void *image, unsigned long len,
                  scan68k_report *out, char *err, unsigned errlen)
{
    const uint8_t *p = image;
    unsigned long i = 0;
    int hunk = 0, numhunks;

    memset(out, 0, sizeof *out);
    if (len < 24 || be32at(p) != HUNK_HEADER) {
        snprintf(err, errlen, "not an AmigaOS hunk executable");
        return 1;
    }
    i = 4;
    /* resident library name strings, terminated by a zero length */
    while (i + 4 <= len) {
        uint32_t n = be32at(p + i); i += 4;
        if (n == 0) break;
        i += (unsigned long)n * 4;
    }
    if (i + 12 > len) { snprintf(err, errlen, "truncated hunk header"); return 1; }
    numhunks = (int)be32at(p + i); i += 12;          /* numhunks, first, last */
    if (numhunks <= 0 || numhunks > 256) {
        snprintf(err, errlen, "implausible hunk count %d", numhunks);
        return 1;
    }
    i += (unsigned long)numhunks * 4;                /* the per-hunk size table */

    /* Walk the hunk stream. A CODE hunk is not scanned when first met: its
     * HUNK_RELOC32 block follows the payload, and the scan needs it to tell a
     * relocated pointer from a real absolute address. So the pending hunk is
     * held and flushed once its relocations are known. */
    {
        const uint8_t *pend_code = NULL;
        unsigned long  pend_bytes = 0;
        int            pend_hunk = 0, have_pend = 0;
        static struct reloc_set rs;

        while (i + 4 <= len) {
            uint32_t t = be32at(p + i) & 0xFFFFFFu;

            if (t == HUNK_CODE || t == HUNK_DATA) {
                unsigned long words, bytes;
                if (i + 8 > len) break;
                if (have_pend) {
                    out->n_code_hunks++; out->code_bytes += pend_bytes;
                    scan_code(pend_code, pend_bytes, pend_hunk, &rs, out);
                    have_pend = 0;
                }
                words = be32at(p + i + 4);
                bytes = words * 4;
                if (i + 8 + bytes > len) bytes = len - (i + 8);
                if (t == HUNK_CODE) {
                    memset(&rs, 0, sizeof rs);
                    pend_code = p + i + 8; pend_bytes = bytes;
                    pend_hunk = hunk; have_pend = 1;
                }
                i += 8 + words * 4;
                hunk++;
            } else if (t == HUNK_BSS) {
                i += 8; hunk++;
            } else if (t == HUNK_RELOC32) {
                i += 4;
                while (i + 8 <= len) {
                    uint32_t cnt = be32at(p + i);
                    if (cnt == 0) { i += 4; break; }
                    i += 8;
                    for (uint32_t c = 0; c < cnt && i + 4 <= len; c++, i += 4)
                        if (have_pend) reloc_mark(&rs, be32at(p + i));
                }
            } else if (t == HUNK_SYMBOL) {
                i += 4;
                while (i + 4 <= len) {
                    uint32_t n = be32at(p + i);
                    if (n == 0) { i += 4; break; }
                    i += 4 + (unsigned long)n * 4 + 4;
                }
            } else if (t == HUNK_DEBUG) {
                if (i + 8 > len) break;
                i += 8 + (unsigned long)be32at(p + i + 4) * 4;
            } else if (t == HUNK_END) {
                i += 4;
            } else {
                break;                    /* unknown: stop, report what we have */
            }
        }
        if (have_pend) {
            out->n_code_hunks++; out->code_bytes += pend_bytes;
            scan_code(pend_code, pend_bytes, pend_hunk, &rs, out);
        }
    }

    if (out->n_code_hunks == 0) {
        snprintf(err, errlen, "no CODE hunk found");
        return 1;
    }

    /* [T3] harvest "<name>.library" strings anywhere in the image: the set of
     * libraries the program can OpenLibrary by name. */
    {
        const uint8_t *q = image;
        unsigned long k;
        for (k = 0; k + 8 < len; k++) {
            if (memcmp(q + k, ".library", 8) != 0) continue;
            unsigned long s0 = k;
            while (s0 > 0 && (q[s0-1] == '.' || q[s0-1] == '-' || q[s0-1] == '_' ||
                   (q[s0-1] >= 'a' && q[s0-1] <= 'z') ||
                   (q[s0-1] >= 'A' && q[s0-1] <= 'Z') ||
                   (q[s0-1] >= '0' && q[s0-1] <= '9')))
                s0--;
            unsigned long nl = k + 8 - s0;
            if (nl < 9 || nl >= 31) { k += 7; continue; }
            if (k + 8 < len && q[k+8] != 0) { k += 7; continue; }  /* want NUL */
            char nm[32];
            memcpy(nm, q + s0, nl); nm[nl] = 0;
            /* The backward walk cannot tell a name's first byte from a
             * preceding opcode byte that happens to be a letter ("rts" ends in
             * 0x75 = 'u'). If a KNOWN classic library name is a suffix of what
             * we grabbed, that is the name. */
            {
                static const char *const known[] = {
                    "exec.library", "dos.library", "intuition.library",
                    "graphics.library", "gadtools.library", "asl.library",
                    "icon.library", "workbench.library", "expansion.library",
                    "utility.library", "layers.library", "diskfont.library",
                    "commodities.library", "keymap.library", "locale.library",
                    "rexxsyslib.library", "translator.library",
                    "datatypes.library", "iffparse.library", "mathffp.library",
                    NULL
                };
                for (int j3 = 0; known[j3]; j3++) {
                    unsigned long kl = strlen(known[j3]);
                    if (nl >= kl && !strcmp(nm + (nl - kl), known[j3])) {
                        memmove(nm, nm + (nl - kl), kl + 1);
                        break;
                    }
                }
            }
            int dup = 0;
            for (int j2 = 0; j2 < out->n_libs; j2++)
                if (!strcmp(out->libs[j2], nm)) { dup = 1; break; }
            if (!dup && out->n_libs < SCAN68K_MAX_LIBS)
                snprintf(out->libs[out->n_libs++], 32, "%s", nm);
            k += 7;
        }
    }

    /* grade: any in-context evidence is a banger; weak evidence only is suspect */
    {
        int j;
        for (j = 0; j < out->n_evidence; j++) {
            if (out->evidence[j].in_context) { out->confidence = SCAN68K_BANGER; break; }
            out->confidence = SCAN68K_SUSPECT;
        }
    }
    return 0;
}

/* what the oscall bridge serves today; grows with emu68k_oscall.c */
/* See the header: the self-reference is the whole test. */
int scan68k_find_resident(const void *image, unsigned long len,
                          unsigned long base, scan68k_resident *out)
{
    const unsigned char *p = image;
    unsigned long o;

    if (!p || !out || len < 26) return 0;
    for (o = 0; o + 26 <= len; o += 2) {
        unsigned long match, init, nameoff;
        if (!(p[o] == 0x4A && p[o + 1] == 0xFC)) continue;      /* RTC_MATCHWORD */
        match = ((unsigned long)p[o+2] << 24) | ((unsigned long)p[o+3] << 16) |
                ((unsigned long)p[o+4] << 8)  |  (unsigned long)p[o+5];
        if (match != base + o) continue;          /* must point AT ITSELF        */
        if (p[o + 12] != 9) continue;             /* rt_Type must be NT_LIBRARY  */

        out->tag_off = o;
        out->flags   = p[o + 10];
        out->version = p[o + 11];
        out->type    = p[o + 12];
        nameoff = ((unsigned long)p[o+14] << 24) | ((unsigned long)p[o+15] << 16) |
                  ((unsigned long)p[o+16] << 8)  |  (unsigned long)p[o+17];
        init    = ((unsigned long)p[o+22] << 24) | ((unsigned long)p[o+23] << 16) |
                  ((unsigned long)p[o+24] << 8)  |  (unsigned long)p[o+25];
        out->init_off = (init >= base && init < base + len) ? init - base : 0;
        out->name[0] = 0;
        if (nameoff >= base && nameoff < base + len) {
            unsigned long i, n = nameoff - base;
            for (i = 0; i + 1 < sizeof out->name && n + i < len && p[n+i]; i++)
                out->name[i] = (char)p[n + i];
            out->name[i] = 0;
        }
        return 1;
    }
    return 0;
}

int scan68k_lib_bridged(const char *name)
{
    /* Read the GENERATED list rather than keep one here. A hand list is wrong
     * the moment a library is imported, and it was: this reported intuition,
     * graphics, layers, icon, diskfont, workbench, utility and locale as
     * unbridged long after they were served, so the prediction for every real
     * GUI program was far bleaker than what actually happens when it runs. */
#define X(lib) if (!strcmp(name, (lib))) return 1;
    EMU68K_SERVABLE_LIBS(X)
#undef X
    return 0;
}

const char *scan68k_route(const scan68k_report *r)
{
    return (r->confidence == SCAN68K_BANGER) ? "FULL" : "JIT";
}

const char *scan68k_confidence_text(scan68k_confidence c)
{
    switch (c) {
    case SCAN68K_CLEAN:   return "no hardware use found";
    case SCAN68K_SUSPECT: return "hardware-shaped data, but no hardware instruction";
    case SCAN68K_BANGER:  return "hits the Amiga hardware directly";
    }
    return "unknown";
}
