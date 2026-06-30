/* ff1_decode.c -- [FF1] AROS C: program: read a JPEG with AROS dos, decode it
 * with libavcodec's mjpeg decoder, convert to RGB24 with libswscale, and report
 * size + pixel format + a checksum. Proves libavcodec + libswscale (built
 * natively for AROS by the crosstools, the codec step after [FF0]) link and run.
 *
 * The file is read with dos Read() instead of libavformat: the image2 demuxer's
 * posixc-backed reads come back empty on this target (av_read_frame -> 0 packets),
 * and a single image needs no container anyway -- feed the bytes straight to the
 * decoder. Output via dos PutStr, plus an explicit dos handle (argv[2]) because
 * opening libavcodec/stdcio redirects the program's Output() (see ff0_main.c).
 *
 *   FF1Decode <jpeg> [resultfile]      (default MacRW:test.jpg)
 */
#include <proto/dos.h>          /* Open/Read/Seek/Close/PutStr; no <stdio.h> */
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
static const char *uhex(unsigned v, char *buf)
{
    static const char *h = "0123456789abcdef";
    int i; for (i = 0; i < 8; i++) buf[i] = h[(v >> ((7 - i) * 4)) & 0xF];
    buf[8] = '\0'; return buf;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "MacRW:test.jpg";
    const AVCodec   *dec = NULL;
    AVCodecContext  *ctx = NULL;
    AVFrame  *frame = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    unsigned char *jbuf = NULL, *rgb[4] = {0,0,0,0};
    int rgbls[4] = {0,0,0,0};
    BPTR fh = 0;
    long jsize = 0;
    int got = 0, rc = 20;
    char b[12];

    if (argc > 2 && argv[2] && argv[2][0]) g_out = Open((CONST_STRPTR)argv[2], MODE_NEWFILE);
    put("[FF1] read "); put(path); put("\n");

    /* read the whole JPEG with AROS dos */
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) { put("[FF1] FAIL: Open\n"); goto done; }
    Seek(fh, 0, OFFSET_END);
    jsize = Seek(fh, 0, OFFSET_BEGINNING);          /* Seek returns the prior position */
    if (jsize <= 0) { put("[FF1] FAIL: empty file\n"); Close(fh); goto done; }
    jbuf = av_malloc((size_t)jsize + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!jbuf) { put("[FF1] FAIL: av_malloc\n"); Close(fh); goto done; }
    if (Read(fh, jbuf, jsize) != jsize) { put("[FF1] FAIL: Read\n"); Close(fh); goto done; }
    Close(fh);
    { long i; for (i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; i++) jbuf[jsize + i] = 0; }
    put("[FF1] bytes="); put(udec((unsigned)jsize, b)); put("\n");

    dec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!dec) { put("[FF1] FAIL: no mjpeg decoder\n"); goto done; }
    ctx = avcodec_alloc_context3(dec);
    if (!ctx || avcodec_open2(ctx, dec, NULL) < 0) { put("[FF1] FAIL: avcodec_open2\n"); goto done; }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) { put("[FF1] FAIL: alloc\n"); goto done; }
    pkt->data = jbuf;
    pkt->size = (int)jsize;

    if (avcodec_send_packet(ctx, pkt) < 0) { put("[FF1] FAIL: send_packet\n"); goto done; }
    avcodec_send_packet(ctx, NULL);                 /* flush */
    if (avcodec_receive_frame(ctx, frame) == 0) got = 1;
    if (!got) { put("[FF1] FAIL: no frame decoded\n"); goto done; }

    put("[FF1] decoded "); put(udec(frame->width, b)); put("x"); put(udec(frame->height, b));
    put(" pixfmt="); put(udec((unsigned)frame->format, b)); put("\n");

    sws = sws_getContext(frame->width, frame->height, frame->format,
                         frame->width, frame->height, AV_PIX_FMT_RGB24,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) { put("[FF1] FAIL: sws_getContext\n"); goto done; }
    if (av_image_alloc(rgb, rgbls, frame->width, frame->height, AV_PIX_FMT_RGB24, 1) < 0) {
        put("[FF1] FAIL: av_image_alloc\n"); goto done; }
    sws_scale(sws, (const unsigned char * const *)frame->data, frame->linesize,
              0, frame->height, rgb, rgbls);

    {
        unsigned sum = 2166136261u; int y, x;
        for (y = 0; y < frame->height; y++)
            for (x = 0; x < frame->width * 3; x++)
                sum = (sum ^ rgb[0][y * rgbls[0] + x]) * 16777619u;
        put("[FF1] RGB24 "); put(udec(frame->width, b)); put("x"); put(udec(frame->height, b));
        put(" fnv1a=0x"); put(uhex(sum, b)); put("\n");
    }

    put("FFMPEG-AROS: [FF1] DECODE OK\n");
    rc = 0;

done:
    if (rgb[0]) av_freep(&rgb[0]);
    if (sws)    sws_freeContext(sws);
    if (frame)  av_frame_free(&frame);
    if (pkt)    av_packet_free(&pkt);
    if (ctx)    avcodec_free_context(&ctx);
    if (jbuf)   av_free(jbuf);
    if (rc) put("FFMPEG-AROS: [FF1] FAIL\n");
    if (g_out) Close(g_out);
    return rc;
}
