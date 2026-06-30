/* aros_avio.h -- the "right way" to give ffmpeg file I/O on AROS: a custom
 * AVIOContext backed by AROS dos (Open/Read/ChangeFilePosition), so libavformat's
 * demuxers read through dos instead of ffmpeg's posixc file protocol (whose reads
 * come back empty on this target). Works for any container (mp4/avi/mkv/image2).
 */
#ifndef AROS_AVIO_H
#define AROS_AVIO_H

#include <libavformat/avformat.h>

/* Open a media file via AROS dos and run avformat_open_input + find_stream_info
 * over a dos-backed AVIOContext. Returns a ready AVFormatContext, or NULL. */
AVFormatContext *aros_avio_open(const char *path);

/* Tear down a context returned by aros_avio_open (closes the dos handle and frees
 * the custom AVIO). Safe on NULL. */
void aros_avio_close(AVFormatContext *fmt);

#endif
