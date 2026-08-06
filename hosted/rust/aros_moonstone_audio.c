/* aros_moonstone_audio.c -- the Moonstone audio shim for AROS: stream the game's
 * software mixer to ahi.device.
 *
 * Companion to aros_moonstone_gfx.c. The Rust side owns the mixer (it fills i16
 * mono frames on demand); this file owns the device. Two AHIST_DYNAMICSAMPLE
 * sounds are played back-to-back on one channel: AHI's SoundFunc hook fires when
 * a buffer starts playing, queues the other one, and publishes its index. The
 * game loop polls aros_ms_audio_free_buffer() once a frame and fills whatever
 * came back, so mixing always happens on the game's own task -- nothing but
 * AHI_SetSound runs in AHI's context.
 *
 * The mode is picked by driver name rather than left to the device's default
 * unit: several audio modes are registered on this port and only one of them
 * has a host behind it. -ffixed-x18 like the other glues.
 */
#include <devices/ahi.h>
#include <exec/memory.h>
#include <proto/ahi.h>
#include <proto/alib.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <utility/hooks.h>

struct Library *AHIBase;   /* the inline AHI stubs call through this */

#define MS_SOUNDS 2        /* the double buffer */
#define MS_CHANNEL 0

static struct MsgPort     *g_port;
static struct AHIRequest  *g_io;
static struct AHIAudioCtrl *g_ctrl;
static WORD               *g_buf[MS_SOUNDS];
static struct AHISampleInfo g_sample[MS_SOUNDS];
static ULONG               g_frames;      /* sample frames per buffer */

/* Published by the hook, consumed by the game loop. g_seq only ever grows; the
 * loop tracks how far it has got in g_seen, so a frame that runs long shows up
 * as a gap (g_starve) instead of silently mixing into a playing buffer. */
static volatile ULONG      g_seq;
static volatile LONG       g_queued;      /* index the hook just queued */
static ULONG               g_seen;
static ULONG               g_starve;
static LONG                g_next;        /* index playing / about to be queued */
static int                 g_open;

static IPTR ms_sound_func(struct Hook *hook, struct AHIAudioCtrl *actrl,
                          struct AHISoundMessage *msg)
{
    g_next ^= 1;
    AHI_SetSound(MS_CHANNEL, (UWORD)g_next, 0, 0, actrl, 0);
    g_queued = g_next;
    g_seq++;
    return 0;
}

static struct Hook g_hook = {
    { NULL, NULL },
    (APTR)HookEntry,        /* amiga.lib trampoline: register args -> C args */
    (APTR)ms_sound_func,
    NULL
};

/* The audio mode whose driver is `coreaudio`, or the best realtime mode if the
 * host driver is not installed (a plain AROS machine, or a boot that never ran
 * AddAudioModes). */
static ULONG ms_pick_mode(void)
{
    ULONG id = AHI_INVALID_ID;

    while ((id = AHI_NextAudioID(id)) != AHI_INVALID_ID)
    {
        char driver[32];

        driver[0] = '\0';
        AHI_GetAudioAttrs(id, NULL,
                          AHIDB_BufferLen, (IPTR)sizeof(driver),
                          AHIDB_Driver,    (IPTR)driver,
                          TAG_DONE);
        if (Stricmp(driver, "coreaudio") == 0)
            return id;
    }
    return AHI_BestAudioID(AHIDB_Realtime, TRUE, TAG_DONE);
}

static void ms_teardown(void)
{
    int i;

    if (g_ctrl)
    {
        /* Stop the mixer before anything is freed: no hook can fire after this. */
        AHI_ControlAudio(g_ctrl, AHIC_Play, FALSE, TAG_DONE);
        AHI_FreeAudio(g_ctrl);
        g_ctrl = NULL;
    }
    for (i = 0; i < MS_SOUNDS; i++)
    {
        if (g_buf[i]) { FreeVec(g_buf[i]); g_buf[i] = NULL; }
    }
    if (AHIBase) { CloseDevice((struct IORequest *)g_io); AHIBase = NULL; }
    if (g_io)   { DeleteIORequest((struct IORequest *)g_io); g_io = NULL; }
    if (g_port) { DeleteMsgPort(g_port); g_port = NULL; }
    g_open = 0;
}

/* Open the device and start playing silence. `rate` is the mixing frequency the
 * caller wants and `frames` the buffer size it would like; both are advisory,
 * the granted rate comes back in *out_rate. Returns the sample frames per
 * buffer (what the caller must write each time), or -1 if there is no audio. */
int aros_ms_audio_open(int rate, int frames, int *out_rate)
{
    ULONG mode, mixfreq = 0, maxplay = 0;
    int i;

    if (g_open)
        return (int)g_frames;

    g_seq = g_seen = g_starve = 0;
    g_next = 0;
    g_queued = 0;

    g_port = CreateMsgPort();
    if (!g_port)
        return -1;
    g_io = (struct AHIRequest *)CreateIORequest(g_port, sizeof(struct AHIRequest));
    if (!g_io)
        goto fail;

    g_io->ahir_Version = 4;
    if (OpenDevice(AHINAME, AHI_NO_UNIT, (struct IORequest *)g_io, 0) != 0)
        goto fail;
    AHIBase = (struct Library *)g_io->ahir_Std.io_Device;

    mode = ms_pick_mode();
    if (mode == AHI_INVALID_ID)
        goto fail;

    g_ctrl = AHI_AllocAudio(AHIA_AudioID,   (IPTR)mode,
                            AHIA_MixFreq,   (IPTR)rate,
                            AHIA_Channels,  1,
                            AHIA_Sounds,    MS_SOUNDS,
                            AHIA_SoundFunc, (IPTR)&g_hook,
                            TAG_DONE);
    if (!g_ctrl)
        goto fail;

    AHI_ControlAudio(g_ctrl, AHIC_MixFreq_Query, (IPTR)&mixfreq, TAG_DONE);
    /* AHIDB_MaxPlaySamples is the shortest buffer AHI can stream without
     * running off the end; we play at the mixing frequency, so it needs no
     * Fs/Fm scaling. */
    AHI_GetAudioAttrs(AHI_INVALID_ID, g_ctrl,
                      AHIDB_MaxPlaySamples, (IPTR)&maxplay,
                      TAG_DONE);
    g_frames = (ULONG)(frames > 0 ? frames : 2048);
    if (g_frames < maxplay)
        g_frames = maxplay;

    for (i = 0; i < MS_SOUNDS; i++)
    {
        g_buf[i] = AllocVec(g_frames * sizeof(WORD), MEMF_PUBLIC | MEMF_CLEAR);
        if (!g_buf[i])
            goto fail;
        g_sample[i].ahisi_Type    = AHIST_M16S;
        g_sample[i].ahisi_Address = g_buf[i];
        g_sample[i].ahisi_Length  = g_frames;
        if (AHI_LoadSound((UWORD)i, AHIST_DYNAMICSAMPLE, &g_sample[i], g_ctrl) != AHIE_OK)
            goto fail;
    }

    if (AHI_ControlAudio(g_ctrl, AHIC_Play, TRUE, TAG_DONE) != AHIE_OK)
        goto fail;

    /* Buffer 0 starts (silent); the hook then queues buffer 1 and hands it to
     * the game to fill, so playback opens with one buffer of silence. */
    AHI_Play(g_ctrl,
             AHIP_BeginChannel, MS_CHANNEL,
             AHIP_Freq,         (IPTR)(mixfreq ? mixfreq : (ULONG)rate),
             AHIP_Vol,          0x10000,
             AHIP_Pan,          0x8000,
             AHIP_Sound,        0,
             AHIP_Offset,       0,
             AHIP_Length,       0,
             AHIP_EndChannel,   0,
             TAG_DONE);

    if (out_rate)
        *out_rate = (int)(mixfreq ? mixfreq : (ULONG)rate);
    g_open = 1;
    return (int)g_frames;

fail:
    ms_teardown();
    return -1;
}

/* The buffer that needs new samples, or NULL when both are busy. Write exactly
 * the frame count aros_ms_audio_open() returned. */
short *aros_ms_audio_free_buffer(void)
{
    ULONG seq, behind;

    if (!g_open)
        return NULL;
    seq = g_seq;
    if (seq == g_seen)
        return NULL;
    behind = seq - g_seen;
    if (behind > 1)
        g_starve += behind - 1;   /* the loop missed a buffer: it replayed */
    g_seen = seq;
    return (short *)g_buf[g_queued & 1];
}

/* How many buffers went out stale because the caller polled too slowly. */
int aros_ms_audio_starved(void)
{
    return (int)g_starve;
}

/* Start counting again. The caller uses this once it has fed the first buffer:
 * everything before that is the silence AHI plays while the game is still
 * loading its sounds, not the game loop falling behind. */
void aros_ms_audio_reset_starve(void)
{
    g_starve = 0;
}

void aros_ms_audio_close(void)
{
    if (g_open || g_port)
        ms_teardown();
}
