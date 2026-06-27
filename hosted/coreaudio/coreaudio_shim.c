/* coreaudio_shim.c — CoreAudio host shim: SPSC lock-free ring + offline
 * (headless, silent) AudioUnit render proof.
 *
 * Implemented from docs/features/coreaudio-audio/spec.md
 * ("Concurrency model — the RT-thread <-> AROS SPSC ring", "The C ABI",
 * "Verification"). Independent work: no third-party implementation source —
 * emulator, agent, driver, or otherwise — was read, searched, or consulted in
 * producing it, and any resemblance to existing implementations is coincidental.
 * Sources used:
 *   [PUB]  Apple AudioToolbox/AudioUnit docs: AudioComponent, AudioUnit,
 *          kAudioUnitSubType_GenericOutput, AURenderCallback / AudioUnitRender,
 *          AudioStreamBasicDescription (LinearPCM float32), the real-time
 *          render-thread contract (no locks / no alloc / no blocking / no
 *          syscalls inside the callback); POSIX pthread_sigmask; RIFF/WAVE.
 *   [PUB]  C11 <stdatomic.h> + single-producer/single-consumer ring theory
 *          (release/acquire on two single-word indices, no CAS, no mutex).
 *   [OURS] hosted/display.c (render-to-file unattended-verify discipline),
 *          hosted/cocoametal/ (flat-C-ABI host-shim shape).
 *
 * The shim owns the ring and the render path; it pulls no AROS headers. The ring
 * stores interleaved int16 stereo PCM (AHIST_S16S). Per the spec default the
 * producer pushes int16 (the AHI mixer's narrowed output) and the RT render
 * callback converts int16 -> float32 for CoreAudio's canonical AudioUnit format,
 * doing nothing but a bounded memcpy-and-scale (no locks/alloc/blocking).
 *
 * "Offline" path: rather than open a live output device (which would make sound
 * and need no entitlement but also gives the agent nothing to read), we drive a
 * kAudioUnitSubType_GenericOutput unit by hand with AudioUnitRender to pull N
 * frames through the very same render callback, then write the pulled frames to
 * a WAV file the harness reads. Same RT-callback ring contract, zero hardware.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include "coreaudio_shim.h"

/* ---- ring geometry -------------------------------------------------------- */
/* One frame = stereo int16 = 2 shorts = 4 bytes. The ring holds `cap` frames in
 * a power-of-two slot array of `cap` frames; head/tail are free-running 32-bit
 * counters masked to index. We keep one slot unused so full vs empty is
 * unambiguous with two indices (the classic SPSC convention): usable frames =
 * cap - 1. */
struct CAContext {
    /* ring storage: cap frames, each 2 int16 (L,R) */
    int16_t        *slots;          /* cap * 2 int16 */
    uint32_t        cap;            /* power of two */
    uint32_t        mask;           /* cap - 1 */
    _Atomic uint32_t head;          /* consumer advances (frames consumed) */
    _Atomic uint32_t tail;          /* producer advances (frames produced) */

    /* CoreAudio offline render path */
    AudioUnit       unit;           /* kAudioUnitSubType_GenericOutput */
    unsigned        rateHz;         /* negotiated sample rate */
    int             started;        /* ca_start called, ca_stop not yet */

    /* diagnostics (R-RT* guards / underrun oracle) */
    _Atomic unsigned long pushed;
    _Atomic unsigned long consumed;
    _Atomic unsigned long underruns;
    _Atomic unsigned long rtAROSCalls;
};

#define CHANNELS 2

/* Round n up to the next power of two (>= 2). */
static uint32_t next_pow2(uint32_t n) {
    uint32_t p = 2;
    while (p < n) p <<= 1;
    return p;
}

/* ---- ABI: open / format / lifecycle --------------------------------------- */

CAContext *ca_open(int ringFrames) {
    if (ringFrames < 2) ringFrames = 2;
    CAContext *c = (CAContext *)calloc(1, sizeof *c);
    if (!c) return NULL;

    /* +1 so that `usable == ringFrames`, then round the slot count to pow2. */
    c->cap   = next_pow2((uint32_t)ringFrames + 1);
    c->mask  = c->cap - 1;
    c->slots = (int16_t *)calloc((size_t)c->cap * CHANNELS, sizeof(int16_t));
    if (!c->slots) { free(c); return NULL; }
    atomic_store_explicit(&c->head, 0, memory_order_relaxed);
    atomic_store_explicit(&c->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&c->pushed, 0, memory_order_relaxed);
    atomic_store_explicit(&c->consumed, 0, memory_order_relaxed);
    atomic_store_explicit(&c->underruns, 0, memory_order_relaxed);
    atomic_store_explicit(&c->rtAROSCalls, 0, memory_order_relaxed);

    /* Build the offline GenericOutput AudioUnit. [PUB] AudioComponentFindNext +
     * AudioComponentInstanceNew + AudioUnitInitialize. */
    AudioComponentDescription desc = {0};
    desc.componentType    = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_GenericOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) { free(c->slots); free(c); return NULL; }
    OSStatus err = AudioComponentInstanceNew(comp, &c->unit);
    if (err != noErr || !c->unit) { free(c->slots); free(c); return NULL; }

    c->rateHz = 44100;
    return c;
}

/* The RT render callback: the CONSUMER side. Reads only the ring (plain host
 * memory) and writes ioData. No locks, no alloc, no blocking, no AROS LVO
 * (rtAROSCalls stays 0). int16 ring -> float32 ioData (the AudioUnit's canonical
 * input format). On ring-empty/short it emits silence for the missing frames and
 * counts an underrun. [PUB] AURenderCallback contract + SPSC acquire/release. */
static OSStatus ca_render_cb(void *inRefCon,
                             AudioUnitRenderActionFlags *ioActionFlags,
                             const AudioTimeStamp *inTimeStamp,
                             UInt32 inBusNumber,
                             UInt32 inNumberFrames,
                             AudioBufferList *ioData) {
    (void)ioActionFlags; (void)inTimeStamp; (void)inBusNumber;
    CAContext *c = (CAContext *)inRefCon;

    /* Output is float32 interleaved stereo (one buffer, 2 channels). */
    float *out = (float *)ioData->mBuffers[0].mData;
    UInt32 want = inNumberFrames;

    /* Acquire the producer's published tail; relaxed read of our own head. */
    uint32_t head = atomic_load_explicit(&c->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&c->tail, memory_order_acquire);
    uint32_t avail = tail - head;                 /* frames available (wraps ok) */

    UInt32 take = (avail < want) ? avail : want;
    const float inv = 1.0f / 32768.0f;

    /* Drain `take` frames from the ring into the float output. */
    for (UInt32 i = 0; i < take; i++) {
        uint32_t idx = (head + i) & c->mask;
        const int16_t *s = &c->slots[(size_t)idx * CHANNELS];
        out[(size_t)i * CHANNELS + 0] = (float)s[0] * inv;
        out[(size_t)i * CHANNELS + 1] = (float)s[1] * inv;
    }
    /* Silence for any frames we could not supply (underrun). */
    for (UInt32 i = take; i < want; i++) {
        out[(size_t)i * CHANNELS + 0] = 0.0f;
        out[(size_t)i * CHANNELS + 1] = 0.0f;
    }
    if (take < want)
        atomic_fetch_add_explicit(&c->underruns, 1, memory_order_relaxed);

    /* Publish the new head: release so the producer that read our head before
     * overwriting these slots sees the consume. */
    atomic_store_explicit(&c->head, head + take, memory_order_release);
    atomic_fetch_add_explicit(&c->consumed, (unsigned long)take, memory_order_relaxed);

    return noErr;
}

int ca_set_format(CAContext *c, unsigned *inOutRateHz) {
    if (!c || !inOutRateHz) return -1;
    unsigned rate = *inOutRateHz ? *inOutRateHz : 44100;

    /* Canonical AudioUnit format: 32-bit float, interleaved stereo LinearPCM.
     * [PUB] AudioStreamBasicDescription. Frame = 2 floats = 8 bytes. */
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate       = (Float64)rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mFramesPerPacket  = 1;
    asbd.mChannelsPerFrame = CHANNELS;
    asbd.mBitsPerChannel   = 32;
    asbd.mBytesPerFrame    = CHANNELS * sizeof(float);   /* interleaved */
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;

    OSStatus err;
    /* Output scope (what GenericOutput emits on AudioUnitRender) and input scope
     * (what its render callback supplies) both set to our float32 stereo. */
    err = AudioUnitSetProperty(c->unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 0, &asbd, sizeof asbd);
    if (err != noErr) return -2;
    err = AudioUnitSetProperty(c->unit, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &asbd, sizeof asbd);
    if (err != noErr) return -3;

    /* Wire the render callback (the consumer). [PUB]
     * kAudioUnitProperty_SetRenderCallback. */
    AURenderCallbackStruct cb = {0};
    cb.inputProc       = ca_render_cb;
    cb.inputProcRefCon = c;
    err = AudioUnitSetProperty(c->unit, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (err != noErr) return -4;

    err = AudioUnitInitialize(c->unit);
    if (err != noErr) return -5;

    c->rateHz = rate;
    *inOutRateHz = rate;        /* offline unit accepts what we ask */
    return 0;
}

int ca_start(CAContext *c) {
    if (!c) return -1;
    /* R-RT3: any host thread that CoreAudio may spawn for the render path must be
     * born with all signals masked so it cannot field AROS's SIGALRM scheduler
     * tick / trap signals. The offline GenericOutput unit renders on the calling
     * thread, so there is no spawned RT thread here — but we keep the mask
     * discipline identical to the grafted live-AUHAL form: block all signals
     * across the arm, then restore, so the structure is the one the spec
     * mandates. [PUB] pthread_sigmask; [AROS] Alsa-bridge mask precedent (spec). */
    sigset_t full, saved;
    sigfillset(&full);
    pthread_sigmask(SIG_BLOCK, &full, &saved);

    c->started = 1;

    pthread_sigmask(SIG_SETMASK, &saved, NULL);
    return 0;
}

void ca_stop(CAContext *c) {
    if (!c) return;
    c->started = 0;
}

void ca_close(CAContext *c) {
    if (!c) return;
    if (c->unit) {
        AudioUnitUninitialize(c->unit);
        AudioComponentInstanceDispose(c->unit);
        c->unit = NULL;
    }
    free(c->slots);
    free(c);
}

/* ---- ABI: producer side --------------------------------------------------- */

int ca_ring_capacity(CAContext *c) {
    if (!c) return 0;
    return (int)(c->cap - 1);            /* one slot reserved (full/empty) */
}

int ca_ring_space(CAContext *c) {
    if (!c) return 0;
    uint32_t head = atomic_load_explicit(&c->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&c->tail, memory_order_relaxed);
    uint32_t used = tail - head;
    return (int)((c->cap - 1) - used);
}

int ca_ring_push(CAContext *c, const short *src, int frames) {
    if (!c || !src || frames <= 0) return 0;

    uint32_t head = atomic_load_explicit(&c->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&c->tail, memory_order_relaxed);
    uint32_t used = tail - head;
    uint32_t free_frames = (c->cap - 1) - used;

    uint32_t n = (uint32_t)frames;
    if (n > free_frames) n = free_frames;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (tail + i) & c->mask;
        int16_t *d = &c->slots[(size_t)idx * CHANNELS];
        d[0] = (int16_t)src[(size_t)i * CHANNELS + 0];
        d[1] = (int16_t)src[(size_t)i * CHANNELS + 1];
    }
    /* Release: publish the bytes written above before the new tail is visible. */
    atomic_store_explicit(&c->tail, tail + n, memory_order_release);
    atomic_fetch_add_explicit(&c->pushed, (unsigned long)n, memory_order_relaxed);
    return (int)n;
}

void ca_get_stats(CAContext *c, CAStats *out) {
    if (!c || !out) return;
    out->pushed      = atomic_load_explicit(&c->pushed, memory_order_relaxed);
    out->consumed    = atomic_load_explicit(&c->consumed, memory_order_relaxed);
    out->underruns   = atomic_load_explicit(&c->underruns, memory_order_relaxed);
    out->rtAROSCalls = atomic_load_explicit(&c->rtAROSCalls, memory_order_relaxed);
}

/* ---- WAV writer (RIFF/WAVE, 16-bit PCM) ----------------------------------- */
/* [PUB] the RIFF/WAVE container is a published standard; this layout is dictated
 * by it, not authored by anyone. We write a 44-byte canonical header then the
 * interleaved int16 frames. Multi-byte fields are little-endian (AArch64 native
 * + WAV's native endianness), written byte-wise so the file is correct on any
 * host endianness. */
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static int wav_write(const char *path, const int16_t *interleaved,
                     uint32_t frames, unsigned rate, unsigned channels) {
    uint32_t dataBytes = frames * channels * (uint32_t)sizeof(int16_t);
    uint8_t h[44];
    memcpy(h + 0,  "RIFF", 4);
    put_u32le(h + 4, 36 + dataBytes);                 /* RIFF chunk size */
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32le(h + 16, 16);                            /* fmt chunk size (PCM) */
    put_u16le(h + 20, 1);                             /* WAVE_FORMAT_PCM */
    put_u16le(h + 22, (uint16_t)channels);
    put_u32le(h + 24, rate);                          /* samplesPerSec */
    put_u32le(h + 28, rate * channels * 2);           /* avgBytesPerSec */
    put_u16le(h + 32, (uint16_t)(channels * 2));      /* blockAlign */
    put_u16le(h + 34, 16);                            /* bitsPerSample */
    memcpy(h + 36, "data", 4);
    put_u32le(h + 40, dataBytes);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(h, 1, sizeof h, f) != sizeof h) { fclose(f); return -1; }
    if (dataBytes && fwrite(interleaved, 1, dataBytes, f) != dataBytes) {
        fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

/* ---- ABI: offline render -> WAV (headless, silent oracle) ------------------ */

int ca_render_to_wav(CAContext *c, const char *wavPath, int frames) {
    if (!c || !wavPath || frames <= 0) return -1;

    /* Pull `frames` through the GenericOutput unit in blocks via AudioUnitRender;
     * each call invokes ca_render_cb (the RT-contract consumer) which drains the
     * ring as float32. We convert that float32 back to int16 for the WAV. */
    const UInt32 BLOCK = 512;
    float *fbuf = (float *)malloc((size_t)BLOCK * CHANNELS * sizeof(float));
    int16_t *pcm = (int16_t *)malloc((size_t)frames * CHANNELS * sizeof(int16_t));
    if (!fbuf || !pcm) { free(fbuf); free(pcm); return -1; }

    /* A reusable AudioBufferList describing one interleaved float32 buffer. */
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = CHANNELS;

    AudioTimeStamp ts = {0};
    ts.mFlags = kAudioTimeStampSampleTimeValid;
    ts.mSampleTime = 0;

    UInt32 done = 0;
    int wrote = 0;
    while (done < (UInt32)frames) {
        UInt32 n = (UInt32)frames - done;
        if (n > BLOCK) n = BLOCK;

        /* Availability gate (DRIVING thread, NOT the RT callback): wait until the
         * producer has published at least `n` frames before we ask the unit to
         * render, so the render callback never finds the ring short -> underruns
         * stay 0. This blocking lives here, on the offline driver thread; the
         * render callback itself still never blocks (R-RT2 intact). Bounded by a
         * generous spin budget so a dead producer can't hang us (the test's
         * watchdog is the outer backstop). One frame = one published unit. */
        {
            uint32_t head = atomic_load_explicit(&c->head, memory_order_relaxed);
            long spins = 0;
            const long SPIN_BUDGET = 2000000;     /* ~2 s at 1 us/park */
            for (;;) {
                uint32_t tail = atomic_load_explicit(&c->tail, memory_order_acquire);
                if (tail - head >= n) break;       /* enough produced */
                if (++spins > SPIN_BUDGET) break;  /* give up -> callback will
                                                       underrun, surfaced in stats */
                usleep(1);
            }
        }

        abl.mBuffers[0].mDataByteSize = n * CHANNELS * (UInt32)sizeof(float);
        abl.mBuffers[0].mData = fbuf;

        AudioUnitRenderActionFlags flags = 0;
        OSStatus err = AudioUnitRender(c->unit, &flags, &ts, 0, n, &abl);
        if (err != noErr) { free(fbuf); free(pcm); return -1; }

        /* float32 -> int16 (round, clamp). */
        for (UInt32 i = 0; i < n * CHANNELS; i++) {
            float v = fbuf[i] * 32767.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            pcm[(size_t)done * CHANNELS + i] = (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
        }
        ts.mSampleTime += n;
        done += n;
        wrote = (int)done;
    }

    if (wav_write(wavPath, pcm, (uint32_t)wrote, c->rateHz, CHANNELS) != 0) {
        free(fbuf); free(pcm); return -1;
    }
    free(fbuf); free(pcm);
    return wrote;
}
