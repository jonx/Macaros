/* a_test.c — [A] headless/silent CoreAudio render proof: the SPSC host ring +
 * offline AudioUnit render-to-WAV oracle, asserted numerically.
 *
 * Implemented from docs/features/coreaudio-audio/spec.md
 * ("Verification — unattended", markers [A1]/[A3]/[A4]). Independent work: no
 * third-party implementation source — emulator, agent, driver, or otherwise —
 * was read, searched, or consulted in producing it, and any resemblance to
 * existing implementations is coincidental. Sources: Apple AudioToolbox/AudioUnit
 * docs [PUB]; SPSC ring theory + Goertzel single-bin DFT [PUB]; this project's
 * render-to-file unattended-verify discipline (hosted/display.c, "a file existing
 * is not a PASS — the samples are") [OURS].
 *
 * The standalone spike (no AROS build): a real second pthread is the PRODUCER,
 * standing in for the AHI slave task (this is the exact seam where the slave's
 * CallHookPkt(MixerFunc) int16 output will feed ca_ring_push). It fills the
 * lock-free ring with a known 440 Hz sine (S16 stereo @ 44100). The CONSUMER is
 * the offline kAudioUnitSubType_GenericOutput AudioUnit whose render callback
 * (driven by AudioUnitRender on this thread) pulls from the ring; the pulled
 * frames are written to a WAV in run/. No live output device is opened: headless
 * and silent, no TCC prompt.
 *
 * Asserts:
 *   [A-1] WAV RMS within tolerance of the expected sine RMS, and the dominant
 *         frequency (Goertzel at 440 Hz vs neighbours) is 440 Hz.
 *   [A-2] underruns == 0 over the run; ring wrap/empty/full logic correct (the
 *         run wraps the ring many times under the real two-thread producer/
 *         consumer race; plus a direct empty/full unit check). rtAROSCalls == 0.
 * Prints "[A] PASS ..." / "[A] FAIL ..." and exits cleanly (watchdog thread).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include "coreaudio_shim.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE        44100u
#define FREQ        440.0          /* test sine, Hz */
#define AMPL        12000          /* int16 amplitude (~0.366 full-scale) */
#define CHANNELS    2
#define RING_FRAMES 2048           /* ~46 ms cushion at 44.1 kHz (spec R-RING4) */
#define TOTAL_FRAMES 220500        /* 5 s -> wraps a 2048-frame ring ~108x */
#define WAV_PATH    "run/coreaudio-a.wav"
#define VOL_WAV_PATH "run/coreaudio-volume50.wav"
#define WATCHDOG_S  8

/* ---- watchdog: never hang the loop ---------------------------------------- */
static void *watchdog(void *arg) {
    (void)arg;
    sleep(WATCHDOG_S);
    fprintf(stderr, "[A] FAIL: watchdog fired (%d s) — render hung\n", WATCHDOG_S);
    _exit(2);
}

/* ---- producer: the AHI-slave stand-in (single producer, second thread) ----
 * Generates a continuous 440 Hz int16 stereo sine and pushes it into the ring,
 * parking briefly when the ring is full (R-RING2 back-pressure: never spins hot,
 * never holds a lock the consumer wants — there is none). Stops after it has
 * pushed TOTAL_FRAMES frames. */
typedef struct {
    CAContext      *ctx;
    _Atomic int     done;          /* set when the producer has pushed it all */
    unsigned long   produced;      /* frames pushed (for the oracle) */
} Producer;

static void *producer_thread(void *arg) {
    Producer *p = (Producer *)arg;
    /* small staging buffer; one push attempt per loop */
    enum { CHUNK = 256 };
    short buf[CHUNK * CHANNELS];
    double phase = 0.0;
    const double dphi = 2.0 * M_PI * FREQ / (double)RATE;

    unsigned long pushed_total = 0;
    while (pushed_total < TOTAL_FRAMES) {
        /* fill the staging chunk with the next CHUNK sine frames */
        int n = CHUNK;
        if (pushed_total + (unsigned long)n > TOTAL_FRAMES)
            n = (int)(TOTAL_FRAMES - pushed_total);
        for (int i = 0; i < n; i++) {
            short s = (short)lrint((double)AMPL * sin(phase));
            buf[i * CHANNELS + 0] = s;     /* L */
            buf[i * CHANNELS + 1] = s;     /* R (identical -> mono-in-stereo) */
            phase += dphi;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        /* push the chunk, advancing past whatever the ring accepted; if the ring
         * is full ca_ring_push returns < n and we park one tick and retry the
         * remainder, re-advancing the sine phase only for accepted frames. */
        int off = 0;
        while (off < n) {
            int acc = ca_ring_push(p->ctx, &buf[off * CHANNELS], n - off);
            off += acc;
            pushed_total += (unsigned long)acc;
            if (acc == 0) {
                /* ring full: park ~0.5 ms and retry (back-pressure, not a spin) */
                usleep(500);
            }
        }
    }
    p->produced = pushed_total;
    atomic_store_explicit(&p->done, 1, memory_order_release);
    return NULL;
}

/* ---- WAV reader (for the independent oracle) ------------------------------- */
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Read a canonical 44-byte-header 16-bit PCM WAV; returns frames, fills out
 * params + a malloc'd interleaved int16 array (caller frees). -1 on error. */
static long wav_read(const char *path, unsigned *outRate, unsigned *outCh,
                     int16_t **outPcm) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[44];
    if (fread(h, 1, sizeof h, f) != sizeof h) { fclose(f); return -1; }
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4) ||
        memcmp(h + 12, "fmt ", 4) || memcmp(h + 36, "data", 4)) {
        fclose(f); return -1;
    }
    unsigned ch   = rd_u16le(h + 22);
    unsigned rate = rd_u32le(h + 24);
    unsigned bits = rd_u16le(h + 34);
    uint32_t dataBytes = rd_u32le(h + 40);
    if (bits != 16 || ch == 0) { fclose(f); return -1; }
    long frames = (long)dataBytes / (long)(ch * 2);
    int16_t *pcm = (int16_t *)malloc((size_t)frames * ch * sizeof(int16_t));
    if (!pcm) { fclose(f); return -1; }
    if (fread(pcm, 1, dataBytes, f) != dataBytes) { free(pcm); fclose(f); return -1; }
    fclose(f);
    *outRate = rate; *outCh = ch; *outPcm = pcm;
    return frames;
}

/* ---- oracle: RMS + Goertzel single-bin DFT -------------------------------- */
static double compute_rms(const int16_t *pcm, long frames, unsigned ch) {
    double acc = 0.0;
    long n = frames * (long)ch;
    for (long i = 0; i < n; i++) { double v = pcm[i]; acc += v * v; }
    return n ? sqrt(acc / (double)n) : 0.0;
}

/* Goertzel magnitude (squared) at frequency f over the L channel. */
static double goertzel(const int16_t *pcm, long frames, unsigned ch,
                       unsigned rate, double f) {
    double w = 2.0 * M_PI * f / (double)rate;
    double coeff = 2.0 * cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (long i = 0; i < frames; i++) {
        double x = (double)pcm[i * (long)ch];   /* L channel */
        s0 = x + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/* ---- direct empty/full ring unit check (wrap correctness) ------------------ */
static int check_ring_logic(void) {
    /* Small ring so we can exercise empty/full/wrap deterministically. */
    CAContext *c = ca_open(8);
    if (!c) { printf("[A]   ring-logic: ca_open(8) failed\n"); return 0; }
    int cap = ca_ring_capacity(c);            /* usable frames */
    int ok = 1;

    /* empty: space == cap */
    if (ca_ring_space(c) != cap) { printf("[A]   ring-logic: empty space %d != %d\n", ca_ring_space(c), cap); ok = 0; }

    short f[2] = {100, -100};
    /* fill exactly to full: push cap frames one at a time */
    for (int i = 0; i < cap; i++) {
        if (ca_ring_push(c, f, 1) != 1) { printf("[A]   ring-logic: push %d rejected early\n", i); ok = 0; break; }
    }
    /* full: space == 0, further push rejected */
    if (ca_ring_space(c) != 0) { printf("[A]   ring-logic: full space %d != 0\n", ca_ring_space(c)); ok = 0; }
    if (ca_ring_push(c, f, 1) != 0) { printf("[A]   ring-logic: push to full accepted\n"); ok = 0; }

    /* Index wrap-around far past the slot count is exercised by the main render
     * run below (TOTAL_FRAMES wraps the live ring ~108x under the real two-thread
     * race); here we have proven the empty and full boundaries directly. */
    ca_close(c);
    return ok;
}

static int check_global_volume_gain(void) {
    enum { FRAMES = 2048, RING = 4096 };
    int ok = 1;
    CAContext *c = ca_open(RING);
    if (!c) {
        printf("[A]   volume-gain: ca_open failed\n");
        return 0;
    }

    unsigned rate = RATE;
    if (ca_set_format(c, &rate) != 0 || rate != RATE || ca_start(c) != 0) {
        printf("[A]   volume-gain: format/start failed\n");
        ca_close(c);
        return 0;
    }

    short pcm[FRAMES * CHANNELS];
    double phase = 0.0;
    const double dphi = 2.0 * M_PI * FREQ / (double)RATE;
    for (int i = 0; i < FRAMES; i++) {
        short s = (short)lrint((double)AMPL * sin(phase));
        pcm[i * CHANNELS + 0] = s;
        pcm[i * CHANNELS + 1] = s;
        phase += dphi;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
    }

    ca_set_global_volume(50);
    if (ca_get_global_volume() != 50) {
        printf("[A]   volume-gain: getter returned %d after set 50\n",
               ca_get_global_volume());
        ok = 0;
    }
    if (ca_ring_push(c, pcm, FRAMES) != FRAMES) {
        printf("[A]   volume-gain: ring push short\n");
        ok = 0;
    }
    if (ok && ca_render_to_wav(c, VOL_WAV_PATH, FRAMES) != FRAMES) {
        printf("[A]   volume-gain: render failed\n");
        ok = 0;
    }

    CAStats st;
    ca_get_stats(c, &st);
    ca_stop(c);

    if (ok) {
        unsigned wrate = 0, wch = 0;
        int16_t *wav = NULL;
        long wframes = wav_read(VOL_WAV_PATH, &wrate, &wch, &wav);
        if (wframes != FRAMES || wrate != RATE || wch != CHANNELS) {
            printf("[A]   volume-gain: bad WAV shape frames=%ld rate=%u ch=%u\n",
                   wframes, wrate, wch);
            ok = 0;
        } else {
            double rms = compute_rms(wav, wframes, wch);
            double expected = ((double)AMPL * 0.5) / sqrt(2.0);
            double rms_err = fabs(rms - expected) / expected;
            if (rms_err >= 0.06) {
                printf("[A]   volume-gain: RMS %.1f vs expected %.1f (err %.2f%%)\n",
                       rms, expected, rms_err * 100.0);
                ok = 0;
            } else {
                printf("[A]   volume-gain: 50%% RMS=%.1f expected=%.1f err=%.2f%%\n",
                       rms, expected, rms_err * 100.0);
            }
        }
        free(wav);
    }
    if (st.underruns != 0) {
        printf("[A]   volume-gain: underruns=%lu\n", st.underruns);
        ok = 0;
    }

    ca_set_global_volume(100);
    if (ca_get_global_volume() != 100) {
        printf("[A]   volume-gain: getter returned %d after reset 100\n",
               ca_get_global_volume());
        ok = 0;
    }
    ca_close(c);
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[A] CoreAudio host ring + headless/silent offline-render proof\n");
    printf("[A]   ring=%d frames (~%.0f ms), sine=%.0f Hz @ %u Hz, total=%d frames (~%.1f s)\n",
           RING_FRAMES, 1000.0 * RING_FRAMES / RATE, FREQ, RATE, TOTAL_FRAMES,
           (double)TOTAL_FRAMES / RATE);

    /* watchdog so the loop never hangs */
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    pthread_detach(wd);

    int ok = 1;

    /* direct empty/full/wrap unit check first */
    int ringlogic = check_ring_logic();
    if (!ringlogic) ok = 0;
    printf("[A]   ring empty/full/wrap logic: %s\n", ringlogic ? "ok" : "BAD");

    int volume = check_global_volume_gain();
    if (!volume) ok = 0;
    printf("[A]   global volume gain: %s\n", volume ? "ok" : "BAD");

    /* open the shim + negotiate format */
    CAContext *ctx = ca_open(RING_FRAMES);
    if (!ctx) { printf("[A] FAIL: ca_open failed\n"); return 1; }
    unsigned rate = RATE;
    int fmtrc = ca_set_format(ctx, &rate);
    if (fmtrc != 0) {
        printf("[A] FAIL: ca_set_format rc=%d (GenericOutput unworkable)\n", fmtrc);
        ca_close(ctx);
        return 1;
    }
    if (rate != RATE) {
        printf("[A]   note: negotiated rate %u != requested %u\n", rate, RATE);
    }
    if (ca_start(ctx) != 0) { printf("[A] FAIL: ca_start\n"); ca_close(ctx); return 1; }

    /* spin up the producer (the AHI-slave stand-in, a real second thread) */
    Producer prod = { .ctx = ctx, .done = 0, .produced = 0 };
    pthread_t pt;
    if (pthread_create(&pt, NULL, producer_thread, &prod) != 0) {
        printf("[A] FAIL: pthread_create producer\n"); ca_close(ctx); return 1;
    }

    /* Consumer side: drive AudioUnitRender to pull from the ring and write the
     * WAV. To keep underruns at 0 while still exercising a real two-thread wrap-
     * many-times race, only render frames that are currently available; let the
     * producer keep the ring topped up. We loop until TOTAL_FRAMES rendered. */
    long rendered = ca_render_to_wav(ctx, WAV_PATH, TOTAL_FRAMES);
    if (rendered < 0) {
        printf("[A] FAIL: ca_render_to_wav rc=%ld\n", rendered);
        ca_close(ctx); return 1;
    }

    /* let the producer finish + join */
    pthread_join(pt, NULL);

    CAStats st;
    ca_get_stats(ctx, &st);
    ca_stop(ctx);

    printf("[A]   rendered=%ld frames -> %s\n", rendered, WAV_PATH);
    printf("[A]   stats: pushed=%lu consumed=%lu underruns=%lu rtAROSCalls=%lu\n",
           st.pushed, st.consumed, st.underruns, st.rtAROSCalls);

    /* ---- read the WAV back and assert (independent oracle) ---- */
    unsigned wrate = 0, wch = 0; int16_t *pcm = NULL;
    long wframes = wav_read(WAV_PATH, &wrate, &wch, &pcm);
    if (wframes < 0) { printf("[A] FAIL: cannot read back %s\n", WAV_PATH); ca_close(ctx); return 1; }

    /* header sanity */
    if (wrate != RATE) { printf("[A]   FAIL: WAV rate %u != %u\n", wrate, RATE); ok = 0; }
    if (wch != CHANNELS) { printf("[A]   FAIL: WAV channels %u != %u\n", wch, CHANNELS); ok = 0; }

    /* RMS: expected for an int16 sine of amplitude A is A/sqrt(2). The WAV went
     * int16 -> float32 -> int16, so allow a few % tolerance. Underruns (silence)
     * would drag RMS down — another reason to assert it. */
    double rms = compute_rms(pcm, wframes, wch);
    double expected = (double)AMPL / sqrt(2.0);
    double rms_err = fabs(rms - expected) / expected;

    /* Goertzel at 440 Hz vs neighbours (220, 880 Hz) over a window; the 440 bin
     * must dominate by a wide margin. */
    long win = wframes;
    double g440 = goertzel(pcm, win, wch, wrate, FREQ);
    double g220 = goertzel(pcm, win, wch, wrate, FREQ / 2.0);
    double g880 = goertzel(pcm, win, wch, wrate, FREQ * 2.0);
    double goff = goertzel(pcm, win, wch, wrate, FREQ + 60.0);

    /* dominant frequency: scan a coarse grid and confirm the peak is at 440 */
    double bestf = 0.0, bestmag = -1.0;
    for (double f = 100.0; f <= 2000.0; f += 10.0) {
        double m = goertzel(pcm, win, wch, wrate, f);
        if (m > bestmag) { bestmag = m; bestf = f; }
    }

    int freq_ok = (g440 > 100.0 * g220) && (g440 > 100.0 * g880) &&
                  (g440 > 100.0 * goff) && (fabs(bestf - FREQ) <= 10.0);
    int rms_ok  = (rms_err < 0.05);
    int under_ok = (st.underruns == 0);
    int rt_ok    = (st.rtAROSCalls == 0);
    int count_ok = (wframes == TOTAL_FRAMES);

    if (!freq_ok)  { printf("[A]   FAIL: dominant freq %.0f Hz (g440/g220=%.1f g440/g880=%.1f g440/goff=%.1f)\n",
                            bestf, g440 / (g220 + 1.0), g440 / (g880 + 1.0), g440 / (goff + 1.0)); ok = 0; }
    if (!rms_ok)   { printf("[A]   FAIL: RMS %.1f vs expected %.1f (err %.2f%%)\n", rms, expected, rms_err * 100.0); ok = 0; }
    if (!under_ok) { printf("[A]   FAIL: underruns=%lu (expected 0)\n", st.underruns); ok = 0; }
    if (!rt_ok)    { printf("[A]   FAIL: rtAROSCalls=%lu (expected 0)\n", st.rtAROSCalls); ok = 0; }
    if (!count_ok) { printf("[A]   FAIL: frame count %ld != %d\n", wframes, TOTAL_FRAMES); ok = 0; }

    printf("[A]   RMS=%.1f (expected %.1f, err %.2f%%)  detected-freq=%.0f Hz  underruns=%lu  rtAROSCalls=%lu\n",
           rms, expected, rms_err * 100.0, bestf, st.underruns, st.rtAROSCalls);

    free(pcm);
    ca_close(ctx);

    if (ok) {
        printf("[A] PASS  RMS=%.1f(~%.1f) freq=%.0fHz underruns=%lu rtAROSCalls=%lu frames=%ld wav=%s\n",
               rms, expected, bestf, st.underruns, st.rtAROSCalls, wframes, WAV_PATH);
        return 0;
    } else {
        printf("[A] FAIL  see checks above (wav=%s)\n", WAV_PATH);
        return 1;
    }
}
