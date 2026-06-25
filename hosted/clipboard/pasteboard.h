/* pasteboard.h — flat C ABI for the NSPasteboard <-> AROS clipboard.device shim.
 *
 * Implemented clean-room from docs/features/clipboard-bridge/spec.md
 * ("The C ABI (pasteboard.h)"). No GPL emulator/agent source (WinUAE/FS-UAE/
 * Amiberry/E-UAE/Janus-UAE, vAmiga, QEMU/SPICE vdagent) was read, searched, or
 * consulted. Sources: Apple Foundation/AppKit docs [PUB] (NSPasteboard,
 * changeCount, NSPasteboardTypeString), POSIX, and this project's own hosted
 * spikes [OURS]. This header is the ONLY contact surface between the AROS side
 * (AROS crosstools) and the host shim (Apple clang): the shim pulls no AROS
 * headers; the AROS side pulls no Cocoa/Foundation headers.
 *
 * All strings crossing this ABI are UTF-8 (macOS-native). The ISO-8859-1 side of
 * the bridge lives entirely on the AROS side of the wall — the transcode helpers
 * below let the AROS side (which holds both the CHRS Latin-1 bytes and these
 * UTF-8 ABI strings) do that conversion; the shim itself stays encoding-agnostic.
 */
#ifndef PASTEBOARD_H
#define PASTEBOARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque AROS handle the host poller signals on a host->AROS change. The host
 * treats these as two integers it passes back to one host->AROS callback symbol;
 * it never dereferences a task pointer itself (it has no AROS ABI). */
typedef struct { void *signal_task; int signal_bit; } PBSignalTarget;

/* --- text (spikes C0..C4) --- */

/* Current pasteboard string as freshly malloc'd UTF-8 (NUL-terminated; *len is
 * the byte length excluding the NUL). Returns 0 and *out=NULL if the pasteboard
 * holds no string-typed item. Returns nonzero on success. Caller frees with
 * host_pb_free. */
int    host_pb_get_text(char **out, size_t *len);

/* Replace the pasteboard with one UTF-8 string item: clearContents first, then
 * set the string. Returns the changeCount value the write produced (the host
 * self-written token for anti-ping-pong), or -1 on failure. len excludes any
 * NUL.
 *
 * Anti-ping-pong (host side): the same call also records this returned
 * changeCount as the "last self-write token" inside the shim, atomically. The
 * poller reads that token and does NOT raise a host->AROS change signal for the
 * delta that only reflects this self-write — so the bridge never echoes its own
 * writes back to AROS. (This is the host half of R-LOOPBREAK; the AROS half — the
 * CBD_CURRENTWRITEID token — still guards the AROS->host direction. See the spec
 * "Spike status vs production contract" note.) */
long   host_pb_set_text(const char *utf8, size_t len);

/* Monotonic NSInteger snapshot; bumps on ANY external pasteboard write. The
 * poll-for-change primitive. Marshalled as host long. */
long   host_pb_change_count(void);

void   host_pb_free(void *p);   /* free a buffer host_pb_get_text returned */

/* --- change poller (spike C3) --- */

/* Start a host background thread that polls host_pb_change_count and, when it
 * differs from the value last seen AND the new value is not the shim's own last
 * self-write token (R-LOOPBREAK, see host_pb_set_text), fires the host->AROS
 * change callback (below) with `target`. interval_ms is the poll cadence.
 * Idempotent / one poller per process. Returns 0 on success.
 *
 * All state shared between this poller pthread and the caller thread (run flag,
 * signal target, callback pointer, self-write token, last-seen count) is C11
 * _Atomic with acquire/release ordering — no plain shared globals cross threads.
 * This follows the shared "foreign host thread wakes AROS task" wake contract
 * (host-wake) referenced by the spec. */
int    host_pb_poller_start(PBSignalTarget target, int interval_ms);
void   host_pb_poller_stop(void);

/* TEST-ONLY: run one poll cycle synchronously on the calling thread (no poller
 * pthread) and return whether it WOULD signal: 1 = an external change fired the
 * callback, 0 = no change OR the delta was the shim's own self-write (suppressed
 * per R-LOOPBREAK). Lets the [C] proof assert anti-ping-pong deterministically
 * without racing the background tick. Applies the same delta + self-write-token
 * rule as the real poller and advances the same last-seen baseline. */
int    host_pb_poller_tick_for_test(void);

/* The poller does NOT call exec.Signal itself (it is a host pthread with no AROS
 * context). It calls back THROUGH this function pointer the AROS side installs at
 * init, whose body runs the actual exec Signal(task, 1<<bit). The AROS side
 * registers it via host_pb_set_signal_cb so the host stays AROS-blind. */
typedef void (*PBSignalFn)(void *signal_task, int signal_bit);
void   host_pb_set_signal_cb(PBSignalFn fn);

/* --- ISO-8859-1 <-> UTF-8 transcode (the R-TRANSCODE requirement) ---
 *
 * These live on the AROS side of the wall conceptually, but are provided here as
 * host-clang helpers so the standalone proof can assert the round-trip, and so
 * the bridge has one canonical implementation. Latin-1 maps 1:1 to U+0000..U+00FF
 * [PUB], so the conversion is a tight table walk, written from scratch (no iconv,
 * no GPL).
 *
 * host_latin1_to_utf8: every Latin-1 byte has a UTF-8 codepoint -> always
 *   lossless. Returns malloc'd NUL-terminated UTF-8; *out_len excludes the NUL.
 *   Returns 0 on success, nonzero on alloc failure. Caller frees with
 *   host_pb_free.
 *
 * host_utf8_to_latin1: UTF-8 -> Latin-1 bytes. Codepoints > U+00FF have no
 *   Latin-1 form; `translit` selects the policy: 0 = reject (return nonzero,
 *   *out=NULL) on any un-mappable codepoint; 1 = best-effort substitute '?'
 *   ("//TRANSLIT"-style). Returns malloc'd NUL-terminated Latin-1 byte buffer;
 *   *out_len excludes the NUL. Returns 0 on success. Caller frees with
 *   host_pb_free. */
int    host_latin1_to_utf8(const unsigned char *latin1, size_t len,
                           char **out, size_t *out_len);
int    host_utf8_to_latin1(const char *utf8, size_t len, int translit,
                           unsigned char **out, size_t *out_len);

/* --- named pasteboard selection (spikes / scheme A; keeps tests off the user's
 *     real clipboard) --- */

/* Redirect every host_pb_* call above to a private, uniquely-named NSPasteboard
 * instead of the general pasteboard. Passing NULL (or never calling this) uses
 * the general pasteboard — the shipping behaviour. The spikes call this with a
 * unique name so they never touch the user's clipboard. Idempotent. */
void   host_pb_use_named(const char *name);

/* --- images (spike C5, later; interface fixed now) --- */
int    host_pb_get_png(void **out, size_t *len);          /* NSPasteboardTypePNG */
long   host_pb_set_png(const void *png, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PASTEBOARD_H */
