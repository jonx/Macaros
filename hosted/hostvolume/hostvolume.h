/* hostvolume.h — flat C ABI for the macOS-folder-as-AROS-volume host glue.
 *
 * Implemented from docs/features/host-volume/spec.md (R-NORM, R-CHARSET,
 * R-ESCAPE, R-SIDECAR). Independent work: no third-party implementation source
 * — emulator, agent, driver, or otherwise — was read, searched, or consulted in
 * producing it, and any resemblance to existing implementations is coincidental.
 * Sources: POSIX + Unicode UAX #15 (NFC/NFD) [PUB]; the AROS emul-handler Do*
 * contract as restated in the spec [AROS]; this project's own hosted spikes +
 * harness conventions [OURS].
 *
 * Hand-authored, neutral. This header is the contact surface the AROS-side
 * emul-handler per-host overlay (built by the AROS crosstools) calls into.
 * The glue pulls NO AROS headers and NO CoreFoundation/Cocoa: normalization
 * and charset are pure CPU work; the sidecar uses only libc file I/O that is
 * already present in the overlay's struct LibCInterface (open/read/write/
 * close/unlink/rename). Hence NO -framework dependency.
 */
#ifndef HOSTVOLUME_H
#define HOSTVOLUME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  R-NORM — Unicode NFC normalization (self-contained, table-driven)
 *  Operates on UTF-8 byte strings (the host side of the bridge). The
 *  handler's canonical form is NFC; everything compared or constructed
 *  passes through hv_to_nfc first so NFC vs NFD inputs collapse to one
 *  deterministic key regardless of what the filesystem stored.
 * ------------------------------------------------------------------ */

/* Normalize a NUL-terminated UTF-8 string to NFC. Writes at most dstcap
 * bytes (always NUL-terminates when dstcap>0). Returns the number of bytes
 * that NFC needs *excluding* the NUL (like snprintf): if the return value
 * is >= dstcap the output was truncated. Pure; no host call. */
size_t hv_to_nfc(const char *utf8, char *dst, size_t dstcap);

/* Convenience: normalize both to NFC and compare for byte-equality.
 * Returns 1 if the two strings denote the same NFC sequence, else 0. */
int hv_nfc_equal(const char *a, const char *b);

/* ------------------------------------------------------------------ *
 *  R-CHARSET / R-ESCAPE — AROS Latin-1 bytes <-> host UTF-8
 *  Latin-1 -> UTF-8 is total. UTF-8 -> Latin-1 is partial; code points
 *  outside U+0000..U+00FF are emitted as a reversible ASCII escape
 *  "%uXXXX" (our marker is '%'); the marker itself is escaped as "%u0025"
 *  so the transform is symmetric and round-trips.
 * ------------------------------------------------------------------ */

/* AROS Latin-1 name bytes -> host UTF-8. Also DECODES our "%uXXXX" escapes
 * (incl. the marker's own "%u0025") back to the real code point, so a host name
 * that was escaped on the way in is reconstructed byte-identically on the way
 * out (genuine round-trip). Returns bytes needed (excl. NUL), snprintf-style. */
size_t hv_latin1_to_utf8(const char *latin1, char *dst, size_t dstcap);

/* Host UTF-8 -> AROS Latin-1 name bytes, escaping un-mappable code points
 * and the marker byte. Returns bytes needed (excl. NUL), snprintf-style. */
size_t hv_utf8_to_latin1(const char *utf8, char *dst, size_t dstcap);

/* ------------------------------------------------------------------ *
 *  R-SIDECAR — ".<name>.amimeta" per-file metadata sidecar
 *
 *  AmigaOS fib_Protection layout (public AmigaOS ABI, restated [PUB]):
 *  low nibble is ACTIVE-LOW delete/execute/write/read; the upper bits are
 *  the AmigaOS-only Archive/Pure/Script/Hold that POSIX rwx cannot hold.
 *  We round-trip the FULL 32-bit word; the handler keeps rwx from st_mode
 *  and OR-s in the AmigaOS-only bits from the sidecar.
 * ------------------------------------------------------------------ */

/* fib_Protection bits (AmigaOS public ABI). Active-low for DEWR. */
#define HV_FIBF_DELETE   (1u << 0)
#define HV_FIBF_EXECUTE  (1u << 1)
#define HV_FIBF_WRITE    (1u << 2)
#define HV_FIBF_READ     (1u << 3)
#define HV_FIBF_ARCHIVE  (1u << 4)   /* AmigaOS-only (no POSIX rwx slot) */
#define HV_FIBF_PURE     (1u << 5)   /* AmigaOS-only */
#define HV_FIBF_SCRIPT   (1u << 6)   /* AmigaOS-only */
#define HV_FIBF_HOLD     (1u << 7)   /* AmigaOS-only */

/* The mask of bits the sidecar exists to preserve (everything POSIX rwx
 * cannot represent: the AmigaOS-only upper bits). */
#define HV_FIBF_AMIGA_ONLY \
    (HV_FIBF_ARCHIVE | HV_FIBF_PURE | HV_FIBF_SCRIPT | HV_FIBF_HOLD)

/* The default protection a fresh file carries (rwxd available => all four
 * active-low bits CLEAR). A file with this protection and an empty comment
 * is "default" and gets NO sidecar. */
#define HV_PROT_DEFAULT  0u

#define HV_COMMENT_MAX   80          /* AmigaOS fib_Comment capacity */

typedef struct {
    uint32_t prot;                   /* full fib_Protection word */
    char     comment[HV_COMMENT_MAX + 1]; /* NUL-terminated Latin-1 (AROS) */
} HVMeta;

/* Build the sidecar path ".<basename>.amimeta" for a data-file path.
 * Handles a trailing slash-free path; writes to dst (snprintf-style return).
 * e.g. "/a/b/foo.txt" -> "/a/b/.foo.txt.amimeta". */
size_t hv_sidecar_path(const char *filepath, char *dst, size_t dstcap);

/* True (1) if a name is itself a sidecar (matches ".*.amimeta") and so must
 * be skipped during enumeration (the spec's is_special_dir extension). */
int hv_is_sidecar_name(const char *name);

/* True (1) if this metadata is the default (no sidecar should exist). */
int hv_meta_is_default(const HVMeta *m);

/* Read the sidecar for filepath into *out. Returns 1 if a sidecar existed
 * and parsed (out filled), 0 if absent (out set to defaults), -1 on a hard
 * I/O/parse error. */
int hv_meta_read(const char *filepath, HVMeta *out);

/* Persist metadata for filepath: if *m is default, removes any existing
 * sidecar (keeps the host dir clean); otherwise writes it atomically
 * (temp file in the same dir + rename). Returns 0 on success, -1 on error. */
int hv_meta_write(const char *filepath, const HVMeta *m);

#ifdef __cplusplus
}
#endif

#endif /* HOSTVOLUME_H */
