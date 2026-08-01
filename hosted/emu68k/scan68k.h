/* scan68k.h — [T2a] the static hardware-use scanner for 68k hunk programs.
 * (OURS, AROS-licensed. No Emu68 source.)
 *
 * Answers "how would this program run here?" BEFORE running it, by one linear
 * pass over the CODE hunks of a raw hunk image looking for the three things a
 * translated (API-level) engine cannot serve:
 *   - privileged / supervisor instructions,
 *   - absolute references into the Amiga custom-chip and CIA ranges,
 *   - stores into the exception-vector page.
 *
 * THE OUTPUT IS A CONFIDENCE-GRADED HINT, NOT A VERDICT. A linear scan cannot
 * tell code from data, sees opcode-shaped constants, and cannot see addresses
 * computed at run time. The runtime guard in the engine is the authority; this
 * exists to route the easy cases without executing anything, and to explain the
 * routing decision to a user (`scan68k` on the host, C:Emu68kWhy in-OS).
 *
 * Both consumers share this core, so what the tool prints is what the router
 * decided. */

#ifndef SCAN68K_H
#define SCAN68K_H

#ifdef __cplusplus
extern "C" {
#endif

/* Confidence in "this program hits the hardware". */
typedef enum {
    SCAN68K_CLEAN   = 0,  /* nothing found: route JIT                          */
    SCAN68K_SUSPECT = 1,  /* hardware-shaped bytes, no instruction context:
                           * route JIT and let the runtime guard decide        */
    SCAN68K_BANGER  = 2   /* hardware access in instruction context, or a
                           * privileged instruction: route FULL                */
} scan68k_confidence;

/* One piece of evidence (what was found and where). */
typedef enum {
    SCAN68K_EV_PRIVILEGED = 0,  /* MOVE to/from SR, RESET, STOP, RTE, MOVE USP */
    SCAN68K_EV_CUSTOM,          /* $DFFxxx custom-chip address                 */
    SCAN68K_EV_CIA,             /* $BFxxxx CIA address                         */
    SCAN68K_EV_VECTOR           /* store into the exception-vector page        */
} scan68k_evkind;

#define SCAN68K_MAX_EVIDENCE 32

typedef struct {
    scan68k_evkind kind;
    int            hunk;        /* CODE hunk index                             */
    unsigned long  offset;      /* byte offset inside that hunk                */
    unsigned long  value;       /* the address or the opcode word              */
    int            in_context;  /* 1 = reached through an absolute EA / a real
                                 * instruction encoding (raises confidence)     */
    char           what[64];    /* human-readable, e.g. "move.w to $DFF180"    */
} scan68k_evidence;

#define SCAN68K_MAX_LIBS    16
#define SCAN68K_MAX_LVOOFF  48

typedef struct {
    scan68k_confidence confidence;
    int                n_evidence;
    scan68k_evidence   evidence[SCAN68K_MAX_EVIDENCE];
    int                n_code_hunks;
    unsigned long      code_bytes;
    int                truncated;   /* more evidence existed than we recorded  */

    /* [T3] the OS surface the program wants, for capability-gap prediction:
     * library name strings found in the image, and the negative offsets of its
     * `jsr d16(a6)` library-call sites (which library each call goes through is
     * a runtime fact - the offsets and the names together are the prediction). */
    char               libs[SCAN68K_MAX_LIBS][32];
    int                n_libs;
    int                lvo_off[SCAN68K_MAX_LVOOFF];   /* negative byte offsets  */
    int                n_lvo_off;
    int                n_lib_calls;                   /* total call sites       */
} scan68k_report;

/* Is this library name bridged to a native implementation yet? Returns 2 for
 * fully, 1 for partially (the common case while coverage grows), 0 for not. */
int scan68k_lib_bridged(const char *name);

/* Scan a raw AmigaOS hunk image. Returns 0 on success, nonzero if the image is
 * not a parseable hunk file (err is set). */
int scan68k_image(const void *image, unsigned long len,
                  scan68k_report *out, char *err, unsigned errlen);

/* The route this report implies: "JIT" or "FULL" (a stable, printable token). */
const char *scan68k_route(const scan68k_report *r);

/* One-line summary of the confidence, for a user-facing explanation. */
const char *scan68k_confidence_text(scan68k_confidence c);

#ifdef __cplusplus
}
#endif
#endif /* SCAN68K_H */
