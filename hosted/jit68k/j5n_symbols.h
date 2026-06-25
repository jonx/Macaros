/* j5n_symbols.h — [J5n] HUNK_SYMBOL -> PC->symbol map for the crash report (OURS,
 * AROS-licensed). Authored from the AmigaOS hunk format (a published standard, [PUB])
 * and the REAL AROS loader's symbol-skip (rom/dos/internalloadseg_aos.c:231-252, cited
 * in j4_loader.c). Contains NO Emu68 source.
 *
 * ============================== WHY THIS EXISTS ================================
 * j4_loader.c SKIPS HUNK_SYMBOL (1008) and HUNK_DEBUG (1009) — correct for execution,
 * but it throws away the label->address map a crash report needs to name the 68k call
 * stack ("fault inside `divide` called from `main`") instead of bare hex PCs. [J5n]
 * parses the SAME file buffer the loader consumed, INDEPENDENTLY (the loader is not
 * touched), into a PC->symbol map. The symbol VALUE in a HUNK_SYMBOL entry is hunk-
 * relative; we add the loaded hunk's runtime base (from the j4_seglist the loader
 * already produced) exactly as a relocation would, so the map is in 68k runtime PCs.
 *
 * The SYMBOL hunk follows a CODE/DATA/BSS hunk and refers to it. Its body is a repeated
 * group { BE32 name-length-in-longs n (0 ends the hunk); n longwords of name (NUL-padded);
 * BE32 value }. Verified against vasm output (mul.s -> symbol `loop` value 0x6).
 *
 * HUNK_DEBUG (line info) is toolchain-dependent and frequently absent; we report what we
 * got (symbols-only is fine). vasm/vlink line-number debug is not parsed in [J5n] (its
 * format is not the simple SAS/C LINE form universally) — flagged honest, symbols suffice
 * for the call-stack names.
 * ============================================================================== */
#ifndef J5N_SYMBOLS_H
#define J5N_SYMBOLS_H

#include <stdint.h>
#include <stddef.h>
#include "j4_hunk.h"   /* j4_seglist: the per-hunk runtime bases the loader produced */

#define J5N_MAX_SYMS   512
#define J5N_SYM_NAMELEN 64

typedef struct {
    char     name[J5N_SYM_NAMELEN];
    uint32_t addr;        /* 68k runtime address (hunk base + symbol value)         */
    uint32_t hunk;        /* which hunk index it belongs to                         */
} j5n_sym;

typedef struct {
    int      n;
    j5n_sym  sym[J5N_MAX_SYMS];
    int      had_symbol_hunk;   /* a HUNK_SYMBOL was present in the file            */
    int      had_debug_hunk;    /* a HUNK_DEBUG was present (not parsed; reported)   */
} j5n_symtab;

/* Parse all HUNK_SYMBOL entries out of the hunk file `buf`/`len`, resolving each value
 * to a 68k runtime address via `seg` (the loaded seglist). Returns 0 always (a file with
 * no symbols yields an empty, valid table — symbols are optional). */
int  j5n_symbols_parse(const uint8_t *buf, size_t len, const j4_seglist *seg,
                       j5n_symtab *out);

/* Find the symbol whose address is the greatest <= pc (the enclosing function), within
 * `window` bytes. Returns the j5n_sym* or NULL. *delta (if non-NULL) = pc - sym->addr. */
const j5n_sym *j5n_symbols_lookup(const j5n_symtab *tab, uint32_t pc, uint32_t window,
                                  uint32_t *delta);

#endif /* J5N_SYMBOLS_H */
