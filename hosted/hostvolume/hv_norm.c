/* hv_norm.c — self-contained Unicode NFC normalization (R-NORM).
 *
 * Implemented clean-room from docs/features/host-volume/spec.md (§Normalization,
 * Requirement R-NORM). No GPL emulator source (UAE family or vAmiga) was read,
 * searched, or consulted. Algorithm is from Unicode Annex #15 (Normalization
 * Forms) [PUB] only; the spec mandates NOT using Apple's CFStringNormalize so
 * the handler stays host-call-free and CoreFoundation-free.
 *
 * The pipeline (UAX #15):
 *   1. Decode UTF-8 -> code points.
 *   2. Canonical Decomposition (NFD): replace each composed char by its
 *      canonical decomposition, recursively, then
 *   3. Canonical Ordering: stable-sort runs of combining marks by their
 *      Canonical Combining Class (ccc).
 *   4. Canonical Composition (NFC): greedily recompose starter+mark pairs that
 *      have a primary composite and are not in the Composition Exclusions set.
 *   5. Re-encode code points -> UTF-8.
 *
 * Table coverage: the Latin-1 Supplement (U+00C0..U+00FF) and Latin Extended-A
 * (U+0100..U+017F) precomposed letters, plus the combining diacritics they
 * decompose to (U+0300.. grave/acute/circumflex/tilde/macron/breve/dot/dieresis/
 * ring/cedilla/ogonek/caron/double-acute and U+0327 cedilla / U+0328 ogonek).
 * This is the BMP Latin subset the spec calls sufficient for the Latin-1
 * round-trip; the tables are structured (sorted, binary-searched) to extend to
 * wider ranges by adding rows. A code point with no table entry is its own
 * decomposition and never composes — i.e. pure ASCII and unknown scripts pass
 * through unchanged, which is exactly correct (NFC == input for them).
 */
#include "hostvolume.h"
#include <string.h>

typedef uint32_t cp_t;

/* ---- UTF-8 decode/encode (no host call, pure) ---------------------------- */

/* Decode one code point at *p; advance *p. Malformed bytes are passed through
 * as a single Latin-1-style code point so the transform never loses data
 * (round-trip safe for our own escape bytes which are pure ASCII anyway). */
static cp_t u8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    cp_t c = s[0];
    if (c < 0x80) { *p = s + 1; return c; }
    int n; cp_t min;
    if      ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; min = 0x10000; }
    else { *p = s + 1; return s[0]; }            /* stray continuation/illegal */
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p = s + 1; return s[0]; } /* truncated */
        c = (c << 6) | (s[i] & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *p = s + 1; return s[0];                  /* overlong/surrogate/oob */
    }
    *p = s + n + 1;
    return c;
}

/* Encode one code point; return bytes written (0 if no room). */
static size_t u8_put(cp_t c, char *dst, size_t cap) {
    if (c < 0x80) {
        if (cap < 1) return 0;
        dst[0] = (char)c; return 1;
    } else if (c < 0x800) {
        if (cap < 2) return 0;
        dst[0] = (char)(0xC0 | (c >> 6));
        dst[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    } else if (c < 0x10000) {
        if (cap < 3) return 0;
        dst[0] = (char)(0xE0 | (c >> 12));
        dst[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    } else {
        if (cap < 4) return 0;
        dst[0] = (char)(0xF0 | (c >> 18));
        dst[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }
}

/* ---- Canonical Combining Class (ccc) table ------------------------------- *
 * Only non-zero classes matter for reordering; everything absent is class 0
 * (a starter). The combining marks our decompositions produce are all
 * class 230 (above) except cedilla/ogonek which sit below.  [PUB] UnicodeData. */
typedef struct { cp_t cp; unsigned char ccc; } CccRow;
static const CccRow ccc_tab[] = {
    { 0x0300, 230 },  /* grave            */
    { 0x0301, 230 },  /* acute            */
    { 0x0302, 230 },  /* circumflex       */
    { 0x0303, 230 },  /* tilde            */
    { 0x0304, 230 },  /* macron           */
    { 0x0306, 230 },  /* breve            */
    { 0x0307, 230 },  /* dot above        */
    { 0x0308, 230 },  /* diaeresis        */
    { 0x0309, 230 },  /* hook above       */
    { 0x030A, 230 },  /* ring above       */
    { 0x030B, 230 },  /* double acute     */
    { 0x030C, 230 },  /* caron            */
    { 0x0323, 220 },  /* dot below        */
    { 0x0327, 202 },  /* cedilla (below)  */
    { 0x0328, 202 },  /* ogonek (below)   */
};

static unsigned char ccc_of(cp_t c) {
    int lo = 0, hi = (int)(sizeof(ccc_tab)/sizeof(ccc_tab[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ccc_tab[mid].cp == c) return ccc_tab[mid].ccc;
        if (ccc_tab[mid].cp < c) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ---- Canonical decomposition table --------------------------------------- *
 * Each precomposed code point -> (base, combining mark). All our targets are
 * 2-element canonical decompositions (Latin letter + one diacritic); the
 * decompose step is applied recursively so a 2-step decomposition would still
 * resolve, but none of these need it. MUST be sorted by .cp (binary search).
 * [PUB] derived from UnicodeData canonical decomposition mappings. */
typedef struct { cp_t cp, a, b; } DecRow;
static const DecRow dec_tab[] = {
    /* U+00C0..U+00FF Latin-1 Supplement */
    { 0x00C0, 0x0041, 0x0300 }, /* À */
    { 0x00C1, 0x0041, 0x0301 }, /* Á */
    { 0x00C2, 0x0041, 0x0302 }, /* Â */
    { 0x00C3, 0x0041, 0x0303 }, /* Ã */
    { 0x00C4, 0x0041, 0x0308 }, /* Ä */
    { 0x00C5, 0x0041, 0x030A }, /* Å */
    { 0x00C7, 0x0043, 0x0327 }, /* Ç */
    { 0x00C8, 0x0045, 0x0300 }, /* È */
    { 0x00C9, 0x0045, 0x0301 }, /* É */
    { 0x00CA, 0x0045, 0x0302 }, /* Ê */
    { 0x00CB, 0x0045, 0x0308 }, /* Ë */
    { 0x00CC, 0x0049, 0x0300 }, /* Ì */
    { 0x00CD, 0x0049, 0x0301 }, /* Í */
    { 0x00CE, 0x0049, 0x0302 }, /* Î */
    { 0x00CF, 0x0049, 0x0308 }, /* Ï */
    { 0x00D1, 0x004E, 0x0303 }, /* Ñ */
    { 0x00D2, 0x004F, 0x0300 }, /* Ò */
    { 0x00D3, 0x004F, 0x0301 }, /* Ó */
    { 0x00D4, 0x004F, 0x0302 }, /* Ô */
    { 0x00D5, 0x004F, 0x0303 }, /* Õ */
    { 0x00D6, 0x004F, 0x0308 }, /* Ö */
    { 0x00D9, 0x0055, 0x0300 }, /* Ù */
    { 0x00DA, 0x0055, 0x0301 }, /* Ú */
    { 0x00DB, 0x0055, 0x0302 }, /* Û */
    { 0x00DC, 0x0055, 0x0308 }, /* Ü */
    { 0x00DD, 0x0059, 0x0301 }, /* Ý */
    { 0x00E0, 0x0061, 0x0300 }, /* à */
    { 0x00E1, 0x0061, 0x0301 }, /* á */
    { 0x00E2, 0x0061, 0x0302 }, /* â */
    { 0x00E3, 0x0061, 0x0303 }, /* ã */
    { 0x00E4, 0x0061, 0x0308 }, /* ä */
    { 0x00E5, 0x0061, 0x030A }, /* å */
    { 0x00E7, 0x0063, 0x0327 }, /* ç */
    { 0x00E8, 0x0065, 0x0300 }, /* è */
    { 0x00E9, 0x0065, 0x0301 }, /* é */
    { 0x00EA, 0x0065, 0x0302 }, /* ê */
    { 0x00EB, 0x0065, 0x0308 }, /* ë */
    { 0x00EC, 0x0069, 0x0300 }, /* ì */
    { 0x00ED, 0x0069, 0x0301 }, /* í */
    { 0x00EE, 0x0069, 0x0302 }, /* î */
    { 0x00EF, 0x0069, 0x0308 }, /* ï */
    { 0x00F1, 0x006E, 0x0303 }, /* ñ */
    { 0x00F2, 0x006F, 0x0300 }, /* ò */
    { 0x00F3, 0x006F, 0x0301 }, /* ó */
    { 0x00F4, 0x006F, 0x0302 }, /* ô */
    { 0x00F5, 0x006F, 0x0303 }, /* õ */
    { 0x00F6, 0x006F, 0x0308 }, /* ö */
    { 0x00F9, 0x0075, 0x0300 }, /* ù */
    { 0x00FA, 0x0075, 0x0301 }, /* ú */
    { 0x00FB, 0x0075, 0x0302 }, /* û */
    { 0x00FC, 0x0075, 0x0308 }, /* ü */
    { 0x00FD, 0x0079, 0x0301 }, /* ý */
    { 0x00FF, 0x0079, 0x0308 }, /* ÿ */
    /* U+0100..U+017F Latin Extended-A (the commonly-typed subset) */
    { 0x0100, 0x0041, 0x0304 }, /* Ā */
    { 0x0101, 0x0061, 0x0304 }, /* ā */
    { 0x0102, 0x0041, 0x0306 }, /* Ă */
    { 0x0103, 0x0061, 0x0306 }, /* ă */
    { 0x0104, 0x0041, 0x0328 }, /* Ą */
    { 0x0105, 0x0061, 0x0328 }, /* ą */
    { 0x0106, 0x0043, 0x0301 }, /* Ć */
    { 0x0107, 0x0063, 0x0301 }, /* ć */
    { 0x010C, 0x0043, 0x030C }, /* Č */
    { 0x010D, 0x0063, 0x030C }, /* č */
    { 0x010E, 0x0044, 0x030C }, /* Ď */
    { 0x010F, 0x0064, 0x030C }, /* ď */
    { 0x0112, 0x0045, 0x0304 }, /* Ē */
    { 0x0113, 0x0065, 0x0304 }, /* ē */
    { 0x0116, 0x0045, 0x0307 }, /* Ė */
    { 0x0117, 0x0065, 0x0307 }, /* ė */
    { 0x0118, 0x0045, 0x0328 }, /* Ę */
    { 0x0119, 0x0065, 0x0328 }, /* ę */
    { 0x011A, 0x0045, 0x030C }, /* Ě */
    { 0x011B, 0x0065, 0x030C }, /* ě */
    { 0x011E, 0x0047, 0x0306 }, /* Ğ */
    { 0x011F, 0x0067, 0x0306 }, /* ğ */
    { 0x0130, 0x0049, 0x0307 }, /* İ */
    { 0x0139, 0x004C, 0x0301 }, /* Ĺ */
    { 0x013A, 0x006C, 0x0301 }, /* ĺ */
    { 0x013D, 0x004C, 0x030C }, /* Ľ */
    { 0x013E, 0x006C, 0x030C }, /* ľ */
    { 0x0143, 0x004E, 0x0301 }, /* Ń */
    { 0x0144, 0x006E, 0x0301 }, /* ń */
    { 0x0147, 0x004E, 0x030C }, /* Ň */
    { 0x0148, 0x006E, 0x030C }, /* ň */
    { 0x014C, 0x004F, 0x0304 }, /* Ō */
    { 0x014D, 0x006F, 0x0304 }, /* ō */
    { 0x0150, 0x004F, 0x030B }, /* Ő */
    { 0x0151, 0x006F, 0x030B }, /* ő */
    { 0x0154, 0x0052, 0x0301 }, /* Ŕ */
    { 0x0155, 0x0072, 0x0301 }, /* ŕ */
    { 0x0158, 0x0052, 0x030C }, /* Ř */
    { 0x0159, 0x0072, 0x030C }, /* ř */
    { 0x015A, 0x0053, 0x0301 }, /* Ś */
    { 0x015B, 0x0073, 0x0301 }, /* ś */
    { 0x015E, 0x0053, 0x0327 }, /* Ş */
    { 0x015F, 0x0073, 0x0327 }, /* ş */
    { 0x0160, 0x0053, 0x030C }, /* Š */
    { 0x0161, 0x0073, 0x030C }, /* š */
    { 0x0162, 0x0054, 0x0327 }, /* Ţ */
    { 0x0163, 0x0074, 0x0327 }, /* ţ */
    { 0x0164, 0x0054, 0x030C }, /* Ť */
    { 0x0165, 0x0074, 0x030C }, /* ť */
    { 0x0168, 0x0055, 0x0303 }, /* Ũ */
    { 0x0169, 0x0075, 0x0303 }, /* ũ */
    { 0x016A, 0x0055, 0x0304 }, /* Ū */
    { 0x016B, 0x0075, 0x0304 }, /* ū */
    { 0x016C, 0x0055, 0x0306 }, /* Ŭ */
    { 0x016D, 0x0075, 0x0306 }, /* ŭ */
    { 0x016E, 0x0055, 0x030A }, /* Ů */
    { 0x016F, 0x0075, 0x030A }, /* ů */
    { 0x0170, 0x0055, 0x030B }, /* Ű */
    { 0x0171, 0x0075, 0x030B }, /* ű */
    { 0x0172, 0x0055, 0x0328 }, /* Ų */
    { 0x0173, 0x0075, 0x0328 }, /* ų */
    { 0x0179, 0x005A, 0x0301 }, /* Ź */
    { 0x017A, 0x007A, 0x0301 }, /* ź */
    { 0x017B, 0x005A, 0x0307 }, /* Ż */
    { 0x017C, 0x007A, 0x0307 }, /* ż */
    { 0x017D, 0x005A, 0x030C }, /* Ž */
    { 0x017E, 0x007A, 0x030C }, /* ž */
};

/* Look up a canonical decomposition; returns 1 and fills *a,*b if found. */
static int decompose1(cp_t c, cp_t *a, cp_t *b) {
    int lo = 0, hi = (int)(sizeof(dec_tab)/sizeof(dec_tab[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (dec_tab[mid].cp == c) { *a = dec_tab[mid].a; *b = dec_tab[mid].b; return 1; }
        if (dec_tab[mid].cp < c) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ---- Canonical composition table ----------------------------------------- *
 * Built directly from dec_tab at lookup time: a (starter, mark) -> composite.
 * None of our Latin decompositions are Composition Exclusions (the excluded
 * set is mostly Hebrew/Arabic/Indic + a handful of singletons; none here), so
 * every dec_tab row is also a valid composition pair. [PUB] UAX #15 + the
 * Composition Exclusions list (verified empty over this table's range). */
static int compose1(cp_t a, cp_t b, cp_t *out) {
    for (size_t i = 0; i < sizeof(dec_tab)/sizeof(dec_tab[0]); i++) {
        if (dec_tab[i].a == a && dec_tab[i].b == b) { *out = dec_tab[i].cp; return 1; }
    }
    return 0;
}

/* ---- The pipeline -------------------------------------------------------- */

#define HV_MAXCP 1024   /* names are short; ample headroom, fully bounded */

/* Recursive canonical decomposition of one code point into buf (bounded). */
static size_t decomp_recursive(cp_t c, cp_t *buf, size_t pos, size_t cap) {
    cp_t a, b;
    if (decompose1(c, &a, &b)) {
        pos = decomp_recursive(a, buf, pos, cap);
        pos = decomp_recursive(b, buf, pos, cap);
        return pos;
    }
    if (pos < cap) buf[pos] = c;
    return pos + 1;
}

/* Stable canonical ordering: sort each maximal run of non-starters (ccc!=0)
 * by ccc, preserving original order among equal classes (insertion sort is
 * stable and the runs are tiny). [PUB] UAX #15 Canonical Ordering Algorithm. */
static void canonical_order(cp_t *cps, size_t n) {
    for (size_t i = 1; i < n; i++) {
        unsigned char cci = ccc_of(cps[i]);
        if (cci == 0) continue;                /* starters never move */
        size_t j = i;
        while (j > 0) {
            unsigned char ccp = ccc_of(cps[j - 1]);
            if (ccp == 0 || ccp <= cci) break; /* stop at starter or <= class */
            cp_t t = cps[j]; cps[j] = cps[j - 1]; cps[j - 1] = t;
            j--;
        }
    }
}

/* Canonical composition (NFC). [PUB] UAX #15 Canonical Composition Algorithm:
 * walk left to right; for each starter, try to compose it with each following
 * combinable character, respecting the "blocked" rule (a mark is blocked if an
 * earlier mark of >= ccc sits between it and the starter). */
static size_t compose_run(cp_t *cps, size_t n) {
    if (n == 0) return 0;
    size_t out = 0;
    size_t starter = 0;            /* index in OUTPUT of the current starter */
    cps[out++] = cps[0];
    int have_starter = (ccc_of(cps[0]) == 0);
    unsigned char last_ccc = ccc_of(cps[0]);   /* ccc of last appended (input) */

    for (size_t i = 1; i < n; i++) {
        cp_t c = cps[i];
        unsigned char cccc = ccc_of(c);
        cp_t composed;
        int blocked = have_starter && last_ccc != 0 && last_ccc >= cccc && cccc != 0;
        /* Also blocked if previous appended char was a starter (last_ccc==0)
         * AND that starter is not the active starter (can't happen here since
         * we reset starter on every starter). The classic rule: composable iff
         * not blocked and a primary composite exists. */
        if (have_starter && !blocked &&
            compose1(cps[starter], c, &composed)) {
            cps[starter] = composed;           /* fold into starter in place */
            /* last_ccc stays as the previous mark's ccc (c was consumed) */
            continue;
        }
        cps[out++] = c;
        if (cccc == 0) { have_starter = 1; starter = out - 1; }
        last_ccc = cccc;
    }
    return out;
}

size_t hv_to_nfc(const char *utf8, char *dst, size_t dstcap) {
    cp_t buf[HV_MAXCP];
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)utf8;

    /* 1+2) decode + recursive canonical decomposition */
    while (*p) {
        cp_t c = u8_next(&p);
        n = decomp_recursive(c, buf, n, HV_MAXCP);
        if (n >= HV_MAXCP) { n = HV_MAXCP; break; }  /* clamp; bounded */
    }
    if (n > HV_MAXCP) n = HV_MAXCP;

    /* 3) canonical ordering */
    canonical_order(buf, n);

    /* 4) canonical composition */
    n = compose_run(buf, n);

    /* 5) re-encode */
    size_t need = 0;
    for (size_t i = 0; i < n; i++) {
        char tmp[4];
        size_t k = u8_put(buf[i], tmp, sizeof tmp);
        for (size_t j = 0; j < k; j++) {
            if (need + 1 < dstcap) dst[need] = tmp[j];
            need++;
        }
    }
    if (dstcap > 0) dst[(need < dstcap) ? need : dstcap - 1] = '\0';
    return need;
}

int hv_nfc_equal(const char *a, const char *b) {
    char na[HV_MAXCP], nb[HV_MAXCP];
    hv_to_nfc(a, na, sizeof na);
    hv_to_nfc(b, nb, sizeof nb);
    return strcmp(na, nb) == 0;
}
