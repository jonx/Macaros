/* c_test.m — standalone proof of the NSPasteboard clipboard host shim ([C]).
 *
 * Implemented clean-room from docs/features/clipboard-bridge/spec.md
 * ("Verification" C0..C3 + "Transcode" R-TRANSCODE). No GPL emulator/agent source
 * (WinUAE/FS-UAE/Amiberry/E-UAE/Janus-UAE, vAmiga, QEMU/SPICE vdagent) was read,
 * searched, or consulted. Models this project's [OURS] marker discipline ("a file
 * existing is not a PASS — the asserted bytes are") from hosted/cocoametal/d1_test.m.
 *
 * Headless-safe and non-hanging: it uses a UNIQUELY-NAMED NSPasteboard (NOT the
 * general pasteboard) so it never touches the user's real clipboard, performs only
 * short synchronous pasteboard touches (no NSRunLoop), and exits 0/nonzero.
 *
 * What it proves:
 *   [C-1] set a known ASCII string via the shim, read it back, assert equal +
 *         changeCount advanced.
 *   [C-2] non-ASCII transcode round-trip: Latin-1 bytes (é ü £) <-> UTF-8 NSString
 *         round-trip byte-exact through the shim's transcode helpers, and the UTF-8
 *         survives a pasteboard set/get unchanged.
 *   [C-3] change detection: set twice; assert changeCount STRICTLY increases each
 *         time (the host->AROS change-signal source).
 *   [C-4] anti-ping-pong (R-LOOPBREAK, host side): after a shim host_pb_set_text,
 *         drive one poll cycle and assert it does NOT fire the host->AROS change
 *         signal (suppressing the bridge's own write); then perform an EXTERNAL
 *         write (a direct NSPasteboard setString: via a second handle, simulating
 *         another macOS app) and assert the next poll cycle DOES fire. This proves
 *         the host poller no longer echoes the bridge's own writes back to AROS.
 *         (The AROS-side CBD_CURRENTWRITEID token still guards the other
 *         direction; see spec "Spike status vs production contract".)
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "pasteboard.h"

/* A pasteboard name unique to this process+run, so concurrent runs and the user's
 * real clipboard are all untouched. */
static char g_pbname[128];

static void make_unique_pbname(void) {
    snprintf(g_pbname, sizeof g_pbname,
             "me.jkn.aros.clipboard.spike.%d.%ld",
             (int)getpid(), (long)time(NULL));
}

/* Hex-dump a byte buffer for diagnostic asserts. */
static void hexcat(char *dst, size_t dstsz, const unsigned char *b, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 < dstsz; i++)
        o += (size_t)snprintf(dst + o, dstsz - o, "%02X ", b[i]);
    if (o && o < dstsz) dst[o - 1] = '\0';
}

/* [C-4] host->AROS change-signal sink: counts how many times the poller's
 * PBSignalFn fired. The shim treats (task,bit) as opaque; here task carries the
 * counter address so we can observe firings without any AROS context. */
static int g_signal_fires = 0;
static void test_signal_cb(void *signal_task, int signal_bit) {
    (void)signal_bit;
    if (signal_task) (*(int *)signal_task)++;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[C] NSPasteboard clipboard host shim + transcode (standalone)\n");

    make_unique_pbname();
    host_pb_use_named(g_pbname);
    printf("[C]   using private pasteboard: %s\n", g_pbname);

    int ok = 1;

    /* ---- [C-1] ASCII set/get round-trip + changeCount advance ---- */
    long cc_before = host_pb_change_count();
    const char *a1 = "AROS<C-1> hello pasteboard 123";
    long cc_set = host_pb_set_text(a1, strlen(a1));

    char *got = NULL; size_t glen = 0;
    int have = host_pb_get_text(&got, &glen);
    int c1_text_ok = (have && got && glen == strlen(a1) &&
                      memcmp(got, a1, glen) == 0);
    int c1_cc_ok   = (cc_set >= 0 && cc_set > cc_before);
    int c1_ok = c1_text_ok && c1_cc_ok;
    if (!c1_ok) ok = 0;
    printf("[C-1] ascii rt: set=\"%s\" got=\"%s\" len=%zu cc %ld->%ld  %s\n",
           a1, got ? got : "(null)", glen, cc_before, cc_set,
           c1_ok ? "ok" : "FAIL");
    host_pb_free(got); got = NULL;

    /* ---- [C-2] non-ASCII transcode round-trip ----
     * Latin-1 source bytes for "é ü £":
     *   é=0xE9, ' '=0x20, ü=0xFC, ' '=0x20, £=0xA3.  (£ has no '//' issue; it is
     *   Latin-1 0xA3 = U+00A3.)  None of these are valid standalone UTF-8 bytes,
     *   so a pass-through (no transcode) would corrupt them. */
    const unsigned char latin1[] = { 0xE9, 0x20, 0xFC, 0x20, 0xA3 };
    size_t latin1_len = sizeof latin1;

    /* Latin-1 -> UTF-8 (always lossless). */
    char *utf8 = NULL; size_t utf8_len = 0;
    int e1 = host_latin1_to_utf8(latin1, latin1_len, &utf8, &utf8_len);

    /* The UTF-8 must be exactly: C3 A9 (é) 20 C3 BC (ü) 20 C2 A3 (£) = 8 bytes. */
    const unsigned char want_utf8[] = { 0xC3,0xA9, 0x20, 0xC3,0xBC, 0x20, 0xC2,0xA3 };
    int utf8_bytes_ok = (e1 == 0 && utf8 && utf8_len == sizeof want_utf8 &&
                         memcmp(utf8, want_utf8, utf8_len) == 0);

    /* The UTF-8 must survive a pasteboard set/get unchanged (it is what crosses
     * the ABI to/from NSPasteboard). */
    int pb_utf8_ok = 0;
    if (e1 == 0 && utf8) {
        host_pb_set_text(utf8, utf8_len);
        char *back = NULL; size_t blen = 0;
        if (host_pb_get_text(&back, &blen) && back &&
            blen == utf8_len && memcmp(back, utf8, blen) == 0)
            pb_utf8_ok = 1;
        host_pb_free(back);
    }

    /* UTF-8 -> Latin-1 (reject mode): must reproduce the original bytes exactly. */
    unsigned char *rt = NULL; size_t rt_len = 0;
    int e2 = (e1 == 0)
           ? host_utf8_to_latin1(utf8, utf8_len, /*translit=*/0, &rt, &rt_len)
           : 1;
    int latin1_rt_ok = (e2 == 0 && rt && rt_len == latin1_len &&
                        memcmp(rt, latin1, latin1_len) == 0);

    int c2_ok = utf8_bytes_ok && pb_utf8_ok && latin1_rt_ok;
    if (!c2_ok) ok = 0;
    {
        char hx_l1[64] = "", hx_u8[64] = "", hx_rt[64] = "";
        hexcat(hx_l1, sizeof hx_l1, latin1, latin1_len);
        hexcat(hx_u8, sizeof hx_u8, (const unsigned char *)(utf8 ? utf8 : ""), utf8_len);
        hexcat(hx_rt, sizeof hx_rt, rt ? rt : (const unsigned char *)"", rt_len);
        printf("[C-2] transcode: latin1=[%s] -> utf8=[%s] (%s)\n",
               hx_l1, hx_u8, utf8_bytes_ok ? "bytes ok" : "BYTES BAD");
        printf("[C-2]   pb utf8 set/get %s ; utf8->latin1=[%s] %s\n",
               pb_utf8_ok ? "ok" : "FAIL", hx_rt,
               latin1_rt_ok ? "rt ok" : "RT BAD");
    }
    host_pb_free(utf8);
    host_pb_free(rt);

    /* ---- [C-3] change detection: two sets, each strictly advances cc ---- */
    long cc0 = host_pb_change_count();
    long w1 = host_pb_set_text("change<C-3> one", 15);
    long cc1 = host_pb_change_count();
    long w2 = host_pb_set_text("change<C-3> two", 15);
    long cc2 = host_pb_change_count();
    /* Each write must strictly increase the count, and the returned write-token
     * must match the observed post-write count (the host self-written token). */
    int c3_ok = (w1 > cc0) && (cc1 == w1) && (w2 > cc1) && (cc2 == w2);
    if (!c3_ok) ok = 0;
    printf("[C-3] change: cc0=%ld set1=%ld cc1=%ld set2=%ld cc2=%ld  %s\n",
           cc0, w1, cc1, w2, cc2,
           c3_ok ? "ok (strictly increasing)" : "FAIL");

    /* ---- [C-4] anti-ping-pong: self-write suppressed, external write fires ----
     * Install the change-signal sink (target.task carries our counter), then:
     *   (a) a shim self-write (host_pb_set_text) must NOT fire the signal — the
     *       poller suppresses the delta equal to the self-write token;
     *   (b) an EXTERNAL write (direct NSPasteboard setString: via a second handle,
     *       not via the shim, so no self-write token is recorded) MUST fire it.
     * We drive the poll cycle synchronously (host_pb_poller_tick_for_test) so the
     * assertion is deterministic and does not race the 200 ms background tick. */
    host_pb_set_signal_cb(test_signal_cb);
    PBSignalTarget tgt = { .signal_task = &g_signal_fires, .signal_bit = 4 };
    /* start()/stop() just install the atomic signal target (task,bit) the tick
     * reads; we immediately stop the background pthread so the test owns the
     * cadence via the synchronous tick. (Any stray firing from the pthread's
     * brief life is washed out by the g_signal_fires=0 reset below.) */
    host_pb_poller_start(tgt, 50);
    host_pb_poller_stop();
    /* Drain: one synchronous tick syncs the baseline (g_last_seen) up to the
     * current changeCount regardless of what start() seeded, so the next ticks
     * measure ONLY the writes [C-4] itself performs. */
    (void)host_pb_poller_tick_for_test();

    /* (a) self-write via the shim — must be SUPPRESSED. */
    g_signal_fires = 0;
    long selftok = host_pb_set_text("self<C-4> write via shim", 23);
    int self_fired = host_pb_poller_tick_for_test();
    int c4_self_ok = (selftok >= 0) && (self_fired == 0) && (g_signal_fires == 0);

    /* (b) external write — a direct NSPasteboard write to the SAME private
     * pasteboard, bypassing the shim (no self-write token recorded). Simulates
     * another macOS app changing the clipboard. Must FIRE exactly once. */
    int ext_fired = 0;
    long cc_after_self = host_pb_change_count();
    @autoreleasepool {
        NSString *nm = [NSString stringWithUTF8String:g_pbname];
        NSPasteboard *ext = [NSPasteboard pasteboardWithName:nm];
        [ext clearContents];
        [ext setString:@"external<C-4> write" forType:NSPasteboardTypeString];
    }
    long cc_after_ext = host_pb_change_count();
    ext_fired = host_pb_poller_tick_for_test();
    int c4_ext_ok = (cc_after_ext > cc_after_self) && (ext_fired == 1) &&
                    (g_signal_fires == 1);

    int c4_ok = c4_self_ok && c4_ext_ok;
    if (!c4_ok) ok = 0;
    printf("[C-4] anti-ping-pong: self-write tok=%ld -> tick fired=%d (want 0) %s ; "
           "external cc %ld->%ld -> tick fired=%d sigs=%d (want 1) %s\n",
           selftok, self_fired, c4_self_ok ? "ok" : "FAIL",
           cc_after_self, cc_after_ext, ext_fired, g_signal_fires,
           c4_ext_ok ? "ok" : "FAIL");

    if (ok) {
        printf("[C] PASS C-1 ascii_rt+cc>%ld ; C-2 latin1<->utf8 byte-exact (é ü £) + pb-utf8 rt ; "
               "C-3 changeCount %ld<%ld<%ld ; C-4 self-write suppressed + external fires (anti-ping-pong)\n",
               cc_before, cc0, cc1, cc2);
        return 0;
    }
    printf("[C] FAIL see [C-1]/[C-2]/[C-3]/[C-4] lines above\n");
    return 1;
}
