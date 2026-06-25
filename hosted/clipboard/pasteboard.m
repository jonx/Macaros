/* pasteboard.m — NSPasteboard <-> AROS clipboard.device host shim.
 *
 * Implemented clean-room from docs/features/clipboard-bridge/spec.md
 * ("Host side (hosted/pasteboard.m)", "The C ABI", "Transcode", "Change-detection
 * model"). No GPL emulator/agent source (WinUAE/FS-UAE/Amiberry/E-UAE/Janus-UAE,
 * vAmiga, QEMU/SPICE vdagent) was read, searched, or consulted. Sources: Apple
 * Foundation/AppKit docs [PUB] (NSPasteboard generalPasteboard / pasteboardWith-
 * Name:, changeCount, stringForType:NSPasteboardTypeString, setString:forType:,
 * clearContents), the published Latin-1<->Unicode 1:1 mapping [PUB], POSIX
 * pthreads, and this project's own marker/host-shim discipline [OURS].
 *
 * This shim owns all NSPasteboard access and the changeCount-poller thread. It
 * pulls NO AROS headers; the only AROS-aware thing it holds is the opaque
 * (signal_task, signal_bit) pair handed in by the AROS side, used solely to fire
 * the installed PBSignalFn callback (never an exec.Signal directly). No Cocoa
 * object outlives a call and there is no NSRunLoop — every text entry is one short
 * synchronous pasteboard touch (the spec's "Non-blocking rule").
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include "pasteboard.h"

/* --- cross-thread anti-ping-pong token (R-LOOPBREAK, host side) ------------ *
 *
 * host_pb_set_text records the changeCount its own write produced here; the
 * poller thread reads it and suppresses the matching delta so the bridge never
 * echoes its own host write back to AROS. This is shared between the caller
 * thread (writer) and the poller pthread (reader), so it is C11 _Atomic with
 * release/acquire ordering — never a plain shared global. Initialised to a value
 * no real changeCount can equal (changeCount is a non-negative monotonic
 * NSInteger), so before the first self-write nothing is suppressed. */
static _Atomic long g_self_write_token = -1;

/* --- pasteboard selection ------------------------------------------------- */

/* Name of the private pasteboard the spikes use; nil => general pasteboard (the
 * shipping default). Guarded only by the single-init discipline; set once at
 * startup before any poller runs, so no extra lock is needed for the spikes. */
static NSString *g_pb_name = nil;

void host_pb_use_named(const char *name) {
    @autoreleasepool {
        g_pb_name = (name && name[0]) ? [NSString stringWithUTF8String:name] : nil;
    }
}

/* The one place we resolve which NSPasteboard to touch. */
static NSPasteboard *pb_handle(void) {
    if (g_pb_name) return [NSPasteboard pasteboardWithName:g_pb_name];
    return [NSPasteboard generalPasteboard];
}

/* --- text ----------------------------------------------------------------- */

int host_pb_get_text(char **out, size_t *len) {
    if (out) *out = NULL;
    if (len) *len = 0;
    @autoreleasepool {
        NSPasteboard *pb = pb_handle();
        NSString *s = [pb stringForType:NSPasteboardTypeString];
        if (s == nil) return 0;                       /* no string-typed item */

        /* Explicit UTF-8 length + bytes (UTF8String is NUL-terminated but we want
         * an exact byte count for binary-safe length reporting). */
        NSUInteger n = [s lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
        char *buf = (char *)malloc((size_t)n + 1);
        if (!buf) return 0;
        const char *u = [s UTF8String];               /* valid for this autoreleasepool */
        memcpy(buf, u, (size_t)n);
        buf[n] = '\0';
        if (out) *out = buf;
        if (len) *len = (size_t)n;
        return 1;
    }
}

long host_pb_set_text(const char *utf8, size_t len) {
    @autoreleasepool {
        if (!utf8) return -1;
        NSString *s = [[NSString alloc] initWithBytes:utf8
                                               length:len
                                             encoding:NSUTF8StringEncoding];
        if (s == nil) return -1;                      /* invalid UTF-8 in */
        NSPasteboard *pb = pb_handle();
        [pb clearContents];                           /* bumps changeCount */
        BOOL ok = [pb setString:s forType:NSPasteboardTypeString];
        if (!ok) return -1;
        /* The host self-written token: the changeCount value our own write
         * produced. Publish it (release) so the poller thread (acquire) will
         * suppress the delta that only reflects this self-write — the host half
         * of R-LOOPBREAK. The AROS-side CBD_CURRENTWRITEID token still guards the
         * other (AROS->host) direction. */
        long tok = (long)[pb changeCount];
        atomic_store_explicit(&g_self_write_token, tok, memory_order_release);
        return tok;
    }
}

long host_pb_change_count(void) {
    @autoreleasepool {
        return (long)[pb_handle() changeCount];
    }
}

void host_pb_free(void *p) {
    free(p);
}

/* --- ISO-8859-1 <-> UTF-8 transcode --------------------------------------- *
 *
 * Latin-1 (ISO-8859-1) is exactly U+0000..U+00FF, one codepoint per byte [PUB].
 * UTF-8 encodes U+0000..U+007F as one byte (0xxxxxxx) and U+0080..U+07FF as two
 * (110xxxxx 10xxxxxx). So Latin-1 -> UTF-8 is: byte < 0x80 stays as-is, else two
 * bytes 0xC0|(c>>6), 0x80|(c&0x3F). The reverse decodes those two forms and
 * rejects (or substitutes) anything that decodes above U+00FF. Written from the
 * encoding definitions, no iconv, no table lifted. */

int host_latin1_to_utf8(const unsigned char *latin1, size_t len,
                        char **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!latin1 && len) return 1;
    /* Worst case every byte >= 0x80 -> 2 UTF-8 bytes. */
    char *buf = (char *)malloc(len * 2 + 1);
    if (!buf) return 1;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = latin1[i];
        if (c < 0x80) {
            buf[o++] = (char)c;
        } else {
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    buf[o] = '\0';
    if (out) *out = buf;
    if (out_len) *out_len = o;
    return 0;
}

int host_utf8_to_latin1(const char *utf8, size_t len, int translit,
                        unsigned char **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!utf8 && len) return 1;
    unsigned char *buf = (unsigned char *)malloc(len + 1);  /* <= len Latin-1 bytes */
    if (!buf) return 1;
    size_t o = 0;
    const unsigned char *u = (const unsigned char *)utf8;
    size_t i = 0;
    while (i < len) {
        unsigned char c = u[i];
        unsigned int cp;
        size_t adv;
        if (c < 0x80) { cp = c; adv = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < len && (u[i+1] & 0xC0) == 0x80) {
            cp = ((unsigned)(c & 0x1F) << 6) | (u[i+1] & 0x3F);
            adv = 2;
        }
        else if ((c & 0xF0) == 0xE0 && i + 2 < len &&
                 (u[i+1] & 0xC0) == 0x80 && (u[i+2] & 0xC0) == 0x80) {
            cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(u[i+1] & 0x3F) << 6) |
                 (u[i+2] & 0x3F);
            adv = 3;
        }
        else if ((c & 0xF8) == 0xF0 && i + 3 < len &&
                 (u[i+1] & 0xC0) == 0x80 && (u[i+2] & 0xC0) == 0x80 &&
                 (u[i+3] & 0xC0) == 0x80) {
            cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(u[i+1] & 0x3F) << 12) |
                 ((unsigned)(u[i+2] & 0x3F) << 6) | (u[i+3] & 0x3F);
            adv = 4;
        }
        else {
            /* Malformed lead/continuation. */
            if (translit) { buf[o++] = (unsigned char)'?'; i += 1; continue; }
            free(buf); return 1;
        }
        if (cp <= 0xFF) {
            buf[o++] = (unsigned char)cp;
        } else {
            if (translit) { buf[o++] = (unsigned char)'?'; }  /* //TRANSLIT fallback */
            else { free(buf); return 1; }                     /* reject: no Latin-1 form */
        }
        i += adv;
    }
    buf[o] = '\0';
    if (out) *out = buf;
    if (out_len) *out_len = o;
    return 0;
}

/* --- change poller -------------------------------------------------------- *
 *
 * One pthread loops: read changeCount; on a delta vs. the last value it saw —
 * UNLESS that new value is the shim's own last self-write token (R-LOOPBREAK,
 * g_self_write_token above) — invoke the installed PBSignalFn(target). It touches
 * only NSPasteboard read APIs and the callback — never AROS memory. R-PBTHREAD:
 * this exercises changeCount off the macOS main thread (the spike's C1 settle
 * point).
 *
 * Thread-safety: every datum this poller shares with the caller thread is C11
 * _Atomic. The run flag, the signal target's two fields, the callback pointer,
 * the interval and the last-seen count are all read/written through atomics with
 * acquire/release ordering — there are NO plain shared globals crossing the
 * thread boundary (the data race the reviewer flagged). PBSignalTarget is two
 * scalars, so it is stored as two atomics rather than a non-atomic struct copy.
 * This is the shared "foreign host thread wakes AROS task" wake contract
 * (host-wake) the spec references. */

static pthread_t        g_poller;
static int              g_poller_started = 0;   /* caller-thread-only: guards start/stop, never read by the poller */

static _Atomic int      g_poller_run     = 0;
static _Atomic(void *)  g_target_task    = NULL;
static _Atomic int      g_target_bit     = 0;
static _Atomic int      g_interval_ms    = 200;
static _Atomic(PBSignalFn) g_signal_cb   = NULL;
static _Atomic long     g_last_seen      = 0;   /* last changeCount the poller acted on */

void host_pb_set_signal_cb(PBSignalFn fn) {
    atomic_store_explicit(&g_signal_cb, fn, memory_order_release);
}

static void *poller_main(void *arg) {
    (void)arg;
    long last = host_pb_change_count();
    atomic_store_explicit(&g_last_seen, last, memory_order_release);
    while (atomic_load_explicit(&g_poller_run, memory_order_acquire)) {
        long cur = host_pb_change_count();
        if (cur != last) {
            last = cur;
            atomic_store_explicit(&g_last_seen, last, memory_order_release);
            /* R-LOOPBREAK (host side): if this new count is exactly the token our
             * own host_pb_set_text produced, the delta is our own write echoing
             * back — do NOT raise a host->AROS change. Only an EXTERNAL write
             * (a different count) fires the signal. */
            long self = atomic_load_explicit(&g_self_write_token, memory_order_acquire);
            if (cur != self) {
                PBSignalFn fn = atomic_load_explicit(&g_signal_cb, memory_order_acquire);
                if (fn) {
                    void *task = atomic_load_explicit(&g_target_task, memory_order_acquire);
                    int   bit  = atomic_load_explicit(&g_target_bit,  memory_order_acquire);
                    fn(task, bit);
                }
            }
        }
        /* Sleep interval_ms in small slices so stop() is responsive. */
        int interval = atomic_load_explicit(&g_interval_ms, memory_order_acquire);
        int slept = 0, slice = 10;
        while (atomic_load_explicit(&g_poller_run, memory_order_acquire) && slept < interval) {
            struct timespec ts = { 0, (long)slice * 1000000L };
            nanosleep(&ts, NULL);
            slept += slice;
        }
    }
    return NULL;
}

int host_pb_poller_start(PBSignalTarget target, int interval_ms) {
    if (g_poller_started) return 0;                   /* idempotent: one per process */
    atomic_store_explicit(&g_target_task, target.signal_task, memory_order_release);
    atomic_store_explicit(&g_target_bit,  target.signal_bit,  memory_order_release);
    atomic_store_explicit(&g_interval_ms, (interval_ms > 0) ? interval_ms : 200,
                          memory_order_release);
    atomic_store_explicit(&g_poller_run, 1, memory_order_release);
    if (pthread_create(&g_poller, NULL, poller_main, NULL) != 0) {
        atomic_store_explicit(&g_poller_run, 0, memory_order_release);
        return 1;
    }
    g_poller_started = 1;
    return 0;
}

void host_pb_poller_stop(void) {
    if (!g_poller_started) return;
    atomic_store_explicit(&g_poller_run, 0, memory_order_release);
    pthread_join(g_poller, NULL);
    g_poller_started = 0;
}

/* Drive one poll cycle SYNCHRONOUSLY on the calling thread, without the poller
 * pthread, and report whether it WOULD have fired the host->AROS change signal.
 * This is the deterministic hook the [C] proof uses to assert R-LOOPBREAK
 * (self-writes suppressed, external writes signalled) without racing a 200 ms
 * background tick. It applies the exact same delta + self-write-token rule as
 * poller_main, updating the same g_last_seen atomic. Returns 1 if it fired (and
 * invokes the installed callback), 0 if it suppressed/no-change. */
int host_pb_poller_tick_for_test(void) {
    long last = atomic_load_explicit(&g_last_seen, memory_order_acquire);
    long cur  = host_pb_change_count();
    if (cur == last) return 0;                        /* no change at all */
    atomic_store_explicit(&g_last_seen, cur, memory_order_release);
    long self = atomic_load_explicit(&g_self_write_token, memory_order_acquire);
    if (cur == self) return 0;                        /* suppressed: our own write */
    PBSignalFn fn = atomic_load_explicit(&g_signal_cb, memory_order_acquire);
    if (fn) {
        void *task = atomic_load_explicit(&g_target_task, memory_order_acquire);
        int   bit  = atomic_load_explicit(&g_target_bit,  memory_order_acquire);
        fn(task, bit);
    }
    return 1;                                         /* fired: external change */
}

/* --- images (spike C5, later; interface fixed, not yet implemented) ------- */

int host_pb_get_png(void **out, size_t *len) {
    if (out) *out = NULL;
    if (len) *len = 0;
    return 0;                                         /* unimplemented: no image item */
}

long host_pb_set_png(const void *png, size_t len) {
    (void)png; (void)len;
    return -1;                                        /* unimplemented (C5, later) */
}
