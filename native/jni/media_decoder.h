/*
 * media_decoder.h -- Android MediaExtractor/MediaCodec based audio decoder.
 *
 * The original Mafon decoder (libmpg.so) is mpg123-only, i.e. MP3 only, while
 * the application lists *every* audio file from MediaStore.  This module adds
 * a fallback decoder built on the NDK media APIs (libmediandk, API 21+), so
 * formats the Android platform can decode (m4a/AAC, flac, ogg/vorbis, opus,
 * wav, ...) become playable through the exact same MP3Decoder JNI surface.
 *
 * Everything is decoded to interleaved 16-bit PCM, stereo, at the stream's
 * native sample rate -- which is what the app's Sonic/AudioTrack pipeline
 * expects (it hardcodes 2 channels).
 */

#ifndef MAFON_MEDIA_DECODER_H
#define MAFON_MEDIA_DECODER_H

#include <stdint.h>

typedef struct media_decoder media_decoder;

/* Open a file and prime the codec (so the output sample rate is known before
 * the app asks for it).  Returns NULL if the file is not decodable by the
 * platform. */
media_decoder *media_decoder_open(const char *path);

/* Read up to `count` interleaved stereo 16-bit samples into dst.
 * Returns the number of samples written, or 0 at end of stream / on error. */
int media_decoder_read(media_decoder *decoder, int16_t *dst, int count);

/* Decode and discard `count` samples; returns the number skipped. */
int media_decoder_skip(media_decoder *decoder, int count);

/* Seek to an absolute position given in frames (samples per channel).
 * Returns the resulting frame position, or -1 on failure. */
int media_decoder_seek(media_decoder *decoder, int frame);

/* Seek relative to the end of stream (matches mpg123_seek(SEEK_END) usage:
 * a positive offset beyond the end fails and returns -1). */
int media_decoder_seek_end(media_decoder *decoder, int delta_frames);

int media_decoder_rate(const media_decoder *decoder);

/* Always 2: the decoded output is up-mixed to stereo. */
int media_decoder_channels(const media_decoder *decoder);

/* Duration in seconds, 0 if unknown. */
float media_decoder_length(const media_decoder *decoder);

void media_decoder_close(media_decoder *decoder);

/* Diagnostic logging (logcat + best-effort file), also used by mpg_jni.c. */
void media_decoder_log(const char *format, ...);

#endif /* MAFON_MEDIA_DECODER_H */
