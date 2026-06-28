/* coreaudio_shim.h — flat C ABI for the CoreAudio-backed AHI sub-driver host shim.
 *
 * Implemented from docs/features/coreaudio-audio/spec.md
 * ("The C ABI (coreaudio_shim.h)" and "Concurrency model"). Independent work:
 * no third-party implementation source — emulator, agent, driver, or otherwise —
 * was read, searched, or consulted in producing it, and any resemblance to
 * existing implementations is coincidental. Sources: Apple AudioToolbox /
 * AudioUnit docs [PUB], C11 atomics + SPSC ring-buffer theory [PUB], and this
 * project's own H-series spikes (the render-to-file unattended-verify stance of
 * hosted/display.c, the flat C ABI shape of hosted/cocoametal/cocoametal.h)
 * [OURS]. No AROS headers are pulled here.
 *
 * Hand-authored, neutral. This header is the *only* contact surface between the
 * AROS-side AHI sub-driver (AROS crosstools) and the host shim (Apple clang).
 * The shim pulls no AROS headers; the AROS side pulls no CoreAudio headers.
 *
 * The shim owns the single SPSC ring and the CoreAudio render thread. The ring
 * carries interleaved 16-bit signed stereo PCM (AHIST_S16S): frame = {L,R},
 * each sample int16 little-endian, frame size 4 bytes. Exactly one producer
 * (the AROS slave task / the test's sine thread) and exactly one consumer (the
 * CoreAudio RT render callback) touch the ring; neither holds a lock.
 */
#ifndef COREAUDIO_SHIM_H
#define COREAUDIO_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CAContext CAContext;

/* Open: create the AudioComponent + offline output AudioUnit (NOT started) and
 * allocate the SPSC ring sized for at least `ringFrames` stereo frames (rounded
 * up to a power of two so wrap is a mask). Returns NULL on failure. */
CAContext *ca_open(int ringFrames);

/* Negotiate the output rate. Caller passes the requested rate in *inOutRateHz;
 * the shim sets the AudioUnit stream format and writes back the rate actually
 * used. The shim's ring input format is int16 stereo interleaved LE; the shim
 * converts to CoreAudio's float32 internally inside the render callback.
 * Returns 0 on success, nonzero on failure. */
int  ca_set_format(CAContext *, unsigned *inOutRateHz);

/* Enable/disable live macOS speaker output for this context. The default is
 * disabled so the host ABI proof remains headless and silent. The AROS AHI
 * bridge enables this before ca_start(), which makes the same render callback
 * feed kAudioUnitSubType_DefaultOutput instead of only the offline WAV oracle. */
int  ca_enable_live_output(CAContext *, int enabled);

/* Arm the render callback. For the offline (headless, silent) proof this just
 * marks the unit renderable; no live output device is opened. The RT render
 * thread is born with all signals masked (R-RT3) for the AROS-host case; in the
 * standalone spike there is no AROS scheduler, but we still mask so the shim is
 * identical to the grafted form. Returns 0 on success. */
int  ca_start(CAContext *);

/* Disarm the render callback. After this returns the render path will not touch
 * the ring again. */
void ca_stop(CAContext *);
void ca_close(CAContext *);

/* PRODUCER side (single producer only). Push up to `frames` stereo int16 frames
 * from `src` (interleaved L,R LE) into the ring. Non-blocking. Returns the
 * number of frames actually accepted (< frames when the ring is full). Never
 * calls into CoreAudio, never blocks, never allocates. */
int  ca_ring_push(CAContext *, const short *src, int frames);

/* PRODUCER side: frames of free space in the ring right now (advisory; may only
 * grow after this returns, never shrink, since the producer is the only
 * writer). Lets the producer decide whether to push or park. */
int  ca_ring_space(CAContext *);

/* PRODUCER side: total capacity of the ring in frames (the usable count). */
int  ca_ring_capacity(CAContext *);

/* Diagnostics for the unattended oracle: monotonic counters the shim maintains.
 *   pushed      — frames accepted by ca_ring_push over the run.
 *   consumed    — frames the render callback pulled out of the ring.
 *   underruns   — times the render callback found the ring empty (or short) and
 *                 emitted silence for the missing frames.
 *   rtAROSCalls — MUST stay 0: a guard the RT path increments if it ever calls
 *                 an AROS LVO (it never does; here it is structurally 0). */
typedef struct { unsigned long pushed, consumed, underruns, rtAROSCalls; } CAStats;
void ca_get_stats(CAContext *, CAStats *out);

/* Process-global master output gain for live/offline CoreAudio contexts.
 * `percent` is clamped to 0..100. It is intentionally global so host UI (the Mac
 * app shell/settings) can change volume without needing a pointer to the
 * currently-open AHI sub-driver instance. */
void ca_set_global_volume(int percent);
int  ca_get_global_volume(void);

/* OFFLINE render for headless verification: pull `frames` stereo frames from the
 * ring by repeatedly driving AudioUnitRender (the GenericOutput unit pulls from
 * the ring via the RT-contract render callback), converting to int16, and
 * writing a 16-bit stereo WAV to `wavPath`. No live device, no sound, no TCC
 * prompt. Returns the number of frames actually written (== frames unless the
 * producer underran and we ran out of input). Returns -1 on a setup error. */
int  ca_render_to_wav(CAContext *, const char *wavPath, int frames);

#ifdef __cplusplus
}
#endif

#endif /* COREAUDIO_SHIM_H */
