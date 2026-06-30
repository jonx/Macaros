/* ff2_show.c -- [FF2] decode a JPEG with libavcodec (as [FF1]) and SHOW the
 * frame in a window on the AROS Workbench desktop: a real video frame, decoded
 * by ffmpeg built natively for AROS, blitted with cybergraphics WritePixelArray.
 *
 *   FF2Show <jpeg> [resultfile]      (default MacRW:test.jpg)
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <dos/dos.h>            /* SIGBREAKF_CTRL_C */
#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

static BPTR g_out = 0;
static void put(const char *s) { PutStr((CONST_STRPTR)s); if (g_out) FPuts(g_out, (CONST_STRPTR)s); }
static const char *udec(unsigned v, char *buf)
{
    char tmp[12]; int i = 0, j = 0;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) buf[j++] = tmp[--i];
    buf[j] = '\0'; return buf;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "MacRW:test.jpg";
    const AVCodec *dec = NULL;
    AVCodecContext *ctx = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    unsigned char *jbuf = NULL, *rgb[4] = {0,0,0,0};
    int rgbls[4] = {0,0,0,0};
    BPTR fh = 0;
    long jsize = 0;
    int got = 0, rc = 20, w = 0, h = 0;
    struct Window *win = NULL;
    char b[12];

    if (argc > 2 && argv[2] && argv[2][0]) g_out = Open((CONST_STRPTR)argv[2], MODE_NEWFILE);

    /* ---- decode (same as FF1) ---- */
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) { put("[FF2] FAIL: Open\n"); goto done; }
    Seek(fh, 0, OFFSET_END);
    jsize = Seek(fh, 0, OFFSET_BEGINNING);
    if (jsize <= 0) { put("[FF2] FAIL: empty\n"); Close(fh); goto done; }
    jbuf = av_malloc((size_t)jsize + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!jbuf || Read(fh, jbuf, jsize) != jsize) { put("[FF2] FAIL: read\n"); Close(fh); goto done; }
    Close(fh);
    { long i; for (i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; i++) jbuf[jsize + i] = 0; }

    dec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    ctx = dec ? avcodec_alloc_context3(dec) : NULL;
    if (!ctx || avcodec_open2(ctx, dec, NULL) < 0) { put("[FF2] FAIL: open codec\n"); goto done; }
    pkt = av_packet_alloc(); frame = av_frame_alloc();
    if (!pkt || !frame) { put("[FF2] FAIL: alloc\n"); goto done; }
    pkt->data = jbuf; pkt->size = (int)jsize;
    avcodec_send_packet(ctx, pkt);
    avcodec_send_packet(ctx, NULL);
    if (avcodec_receive_frame(ctx, frame) == 0) got = 1;
    if (!got) { put("[FF2] FAIL: no frame\n"); goto done; }
    w = frame->width; h = frame->height;
    put("[FF2] decoded "); put(udec(w, b)); put("x"); put(udec(h, b)); put("\n");

    sws = sws_getContext(w, h, frame->format, w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws || av_image_alloc(rgb, rgbls, w, h, AV_PIX_FMT_RGB24, 1) < 0) { put("[FF2] FAIL: sws\n"); goto done; }
    sws_scale(sws, (const unsigned char * const *)frame->data, frame->linesize, 0, h, rgb, rgbls);

    /* ---- show it in a window on the Workbench screen ---- */
    win = OpenWindowTags(NULL,
        WA_Title,       (IPTR)"ffmpeg on AROS -- decoded JPEG",
        WA_InnerWidth,  (IPTR)w,
        WA_InnerHeight, (IPTR)h,
        WA_Left,        (IPTR)40,
        WA_Top,         (IPTR)30,
        WA_Flags,       (IPTR)(WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET | WFLG_ACTIVATE),
        WA_IDCMP,       (IPTR)IDCMP_CLOSEWINDOW,
        TAG_DONE);
    if (!win) { put("[FF2] FAIL: OpenWindow\n"); goto done; }

    WritePixelArray(rgb[0], 0, 0, rgbls[0], win->RPort,
                    win->BorderLeft, win->BorderTop, w, h, RECTFMT_RGB);

    put("FFMPEG-AROS: [FF2] FRAME SHOWN\n");
    rc = 0;
    if (g_out) { Close(g_out); g_out = 0; }   /* flush the result before we block */

    /* keep the window up until its close gadget (or CTRL-C) */
    Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C);

done:
    if (win)    CloseWindow(win);
    if (rgb[0]) av_freep(&rgb[0]);
    if (sws)    sws_freeContext(sws);
    if (frame)  av_frame_free(&frame);
    if (pkt)    av_packet_free(&pkt);
    if (ctx)    avcodec_free_context(&ctx);
    if (jbuf)   av_free(jbuf);
    if (g_out)  { if (rc) put("FFMPEG-AROS: [FF2] FAIL\n"); Close(g_out); }
    return rc;
}
