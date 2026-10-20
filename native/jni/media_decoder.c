/*
 * media_decoder.c -- audio decoding through Android's NDK media APIs.
 *
 * See media_decoder.h for the rationale.  Design notes:
 *
 *  - One AMediaExtractor drives the container; the first audio track is
 *    selected.  We do not assume a particular container or codec: whatever
 *    the platform advertises (m4a/AAC, flac, ogg, opus, wav, ...) is used.
 *  - AMediaCodec runs in synchronous mode: dequeue input -> feed from the
 *    extractor, dequeue output -> convert to interleaved signed 16-bit PCM.
 *    The codec may emit 16-bit or float PCM depending on device/format, so the
 *    output format's pcm-encoding is honoured.
 *  - Mono is duplicated to stereo and multichannel is reduced to its first
 *    two channels, because the app's playback pipeline is stereo-only.
 *  - Decoded samples are staged in a small growable buffer so readSamples()
 *    can be served in arbitrary chunk sizes (the app reads 16384 shorts at a
 *    time).
 *  - The first output buffer is decoded during open so that the real output
 *    sample rate (e.g. SBR-doubled HE-AAC) is known before the app calls
 *    getRate() and creates its AudioTrack/Sonic chain.
 *
 * Logging tag: "mafon-mpg".
 */

#include "media_decoder.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MAFON_MEDIA_DEBUG
#include <stdio.h>
#define DBG(...) do { fprintf(stderr, "[media_decoder] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#else
#define DBG(...) do { } while (0)
#endif

#define LOG_TAG "mafon-mpg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* AudioFormat.ENCODING_* values used by MediaFormat's "pcm-encoding". */
#define PCM_ENCODING_16BIT 2
#define PCM_ENCODING_8BIT 3
#define PCM_ENCODING_FLOAT 4

/* AMEDIAFORMAT_KEY_PCM_ENCODING is only declared for API 28+, but the key
 * itself has existed since the key/value MediaFormat interface was introduced;
 * use the literal so the library keeps working (and links) on API 21+. */
#define MAFON_KEY_PCM_ENCODING "pcm-encoding"

#define PCM_BUFFER_INITIAL_BYTES (64 * 1024)
#define DEQUEUE_TIMEOUT_US 5000
#define MAX_IDLE_LOOPS 400 /* ~2 s of waiting for the EOS buffer */

/* Diagnostic log: Android logcat plus a best-effort file, because some ROMs
 * disable adb logcat entirely.  The file lives in shared storage (the app has
 * legacy external storage) with a fallback to the app's own external dir. */
static void media_log_write(const char *line)
{
    static const char *const paths[] = {
        "/storage/emulated/0/Android/data/com.neosphere.mafon/files/mafon_media.log",
        "/sdcard/Download/mafon_media.log",
        NULL
    };
    static int path_index = -1;
    FILE *file;
    int i;

    if (path_index >= 0) {
        file = fopen(paths[path_index], "a");
        if (file != NULL) {
            fputs(line, file);
            fclose(file);
            return;
        }
        path_index = -1;
    }
    for (i = 0; paths[i] != NULL; i++) {
        file = fopen(paths[i], "a");
        if (file != NULL) {
            path_index = i;
            fputs(line, file);
            fclose(file);
            return;
        }
    }
}

void media_decoder_log(const char *format, ...)
{
    va_list args;
    char buffer[1024];

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", buffer);

    {
        char line[1200];
        snprintf(line, sizeof(line), "[mafon] %s\n", buffer);
        media_log_write(line);
    }
}

struct media_decoder {
    AMediaExtractor *extractor;
    AMediaCodec *codec;
    AMediaFormat *track_format;

    int rate_in;
    int channels_in;
    int64_t duration_us;

    int rate_out;
    int channels_out;
    int pcm_encoding;

    int input_eos;
    int output_eos;

    int16_t *pcm;      /* interleaved stereo 16-bit staging buffer */
    size_t pcm_capacity; /* bytes */
    int pcm_samples;   /* samples currently staged */
    int pcm_pos;       /* samples already consumed */
};

/* Open the media source by file descriptor when possible: on recent Android
 * releases MediaExtractor may hand demuxing to a separate service process, so a
 * path handed to setDataSource() can be inaccessible to that service.  Opening
 * the fd in our own process and passing it avoids that class of failures. */
static media_status_t set_data_source(AMediaExtractor *extractor, const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        off64_t length = 0;
        if (fstat(fd, &st) == 0 && st.st_size > 0)
            length = (off64_t)st.st_size;
        media_status_t status = AMediaExtractor_setDataSourceFd(extractor, fd, 0, length);
        DBG("setDataSourceFd(%d, len=%lld) -> %d", fd, (long long)length, (int)status);
        if (status == AMEDIA_OK)
            return status; /* the extractor owns the fd now */
        close(fd);
    } else {
        DBG("open(%s) failed: errno=%d", path, errno);
    }
    return AMediaExtractor_setDataSource(extractor, path);
}

static int32_t format_int32(AMediaFormat *format, const char *key, int32_t fallback)
{
    int32_t value = 0;
    if (format != NULL && AMediaFormat_getInt32(format, key, &value))
        return value;
    return fallback;
}

static void apply_output_format(media_decoder *decoder)
{
    AMediaFormat *format = AMediaCodec_getOutputFormat(decoder->codec);
    if (format == NULL)
        return;

    int32_t rate = format_int32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, 0);
    int32_t channels = format_int32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 0);
    int32_t encoding = format_int32(format, MAFON_KEY_PCM_ENCODING, PCM_ENCODING_16BIT);

    if (rate > 0)
        decoder->rate_out = rate;
    if (channels > 0)
        decoder->channels_out = channels;
    decoder->pcm_encoding = encoding;

    media_decoder_log("output format: rate=%d channels=%d pcm-encoding=%d",
                     decoder->rate_out, decoder->channels_out, decoder->pcm_encoding);
    AMediaFormat_delete(format);
}

static int ensure_pcm_capacity(media_decoder *decoder, size_t needed_bytes)
{
    if (needed_bytes <= decoder->pcm_capacity)
        return 1;
    size_t size = decoder->pcm_capacity ? decoder->pcm_capacity : PCM_BUFFER_INITIAL_BYTES;
    while (size < needed_bytes)
        size *= 2;
    int16_t *buffer = (int16_t *)realloc(decoder->pcm, size);
    if (buffer == NULL) {
        LOGE("out of memory growing PCM buffer to %u bytes", (unsigned)size);
        return 0;
    }
    decoder->pcm = buffer;
    decoder->pcm_capacity = size;
    return 1;
}

static int16_t clamp_to_int16(float value)
{
    if (value > 32767.0f)
        return 32767;
    if (value < -32768.0f)
        return -32768;
    return (int16_t)value;
}

/* Convert one codec output buffer (16-bit, 8-bit or float PCM, any channel
 * count) into interleaved stereo 16-bit PCM inside decoder->pcm. */
static int convert_output(media_decoder *decoder, const uint8_t *data, size_t size)
{
    int channels = decoder->channels_out > 0 ? decoder->channels_out : 1;
    int encoding = decoder->pcm_encoding;
    size_t bytes_per_sample;

    if (encoding == PCM_ENCODING_FLOAT)
        bytes_per_sample = 4;
    else if (encoding == PCM_ENCODING_8BIT)
        bytes_per_sample = 1;
    else
        bytes_per_sample = 2;

    size_t frames = size / (bytes_per_sample * (size_t)channels);
    if (frames == 0)
        return 0;

    if (!ensure_pcm_capacity(decoder, frames * 2 * sizeof(int16_t)))
        return 0;

    int16_t *out = decoder->pcm;

    for (size_t frame = 0; frame < frames; frame++) {
        int16_t left;
        int16_t right;

        if (encoding == PCM_ENCODING_FLOAT) {
            const float *samples = (const float *)data + frame * (size_t)channels;
            left = clamp_to_int16(samples[0] * 32767.0f);
            right = (channels > 1) ? clamp_to_int16(samples[1] * 32767.0f) : left;
        } else if (encoding == PCM_ENCODING_8BIT) {
            const uint8_t *samples = data + frame * (size_t)channels;
            left = (int16_t)((int)samples[0] - 128) << 8;
            right = (channels > 1) ? (int16_t)((int)samples[1] - 128) << 8 : left;
        } else {
            const int16_t *samples = (const int16_t *)data + frame * (size_t)channels;
            left = samples[0];
            right = (channels > 1) ? samples[1] : left;
        }

        out[frame * 2] = left;
        out[frame * 2 + 1] = right;
    }

    decoder->pcm_samples = (int)(frames * 2);
    decoder->pcm_pos = 0;
    return 1;
}

/* Decode until at least one stereo sample pair is staged.
 * Returns 1 when data is available, 0 at end of stream or on error. */
static int refill(media_decoder *decoder)
{
    if (decoder->pcm_pos < decoder->pcm_samples)
        return 1;

    decoder->pcm_pos = 0;
    decoder->pcm_samples = 0;

    if (decoder->output_eos)
        return 0;

    int idle = 0;
    int loop = 0;
    while (idle < MAX_IDLE_LOOPS) {
        /* Hard bound: never let a misbehaving codec block playback forever.
         * ~2000 iterations x 5 ms is an upper bound of a few seconds. */
        if (++loop > 2000) {
            media_decoder_log("refill: giving up after %d iterations", loop);
            return 0;
        }
        if (loop < 60)
            DBG("refill loop %d: input_eos=%d", loop, decoder->input_eos);
        if (!decoder->input_eos) {
            ssize_t input_index = AMediaCodec_dequeueInputBuffer(decoder->codec, DEQUEUE_TIMEOUT_US);
            if (loop < 60)
                DBG("  dequeueInputBuffer -> %d", (int)input_index);
            if (input_index >= 0) {
                size_t capacity = 0;
                uint8_t *input = AMediaCodec_getInputBuffer(decoder->codec, (size_t)input_index, &capacity);
                if (input == NULL) {
                    LOGE("getInputBuffer failed");
                    return 0;
                }
                ssize_t read = AMediaExtractor_readSampleData(decoder->extractor, input, capacity);
                if (loop < 60)
                    DBG("  readSampleData(cap=%u) -> %d", (unsigned)capacity, (int)read);
                if (read < 0) {
                    media_status_t q = AMediaCodec_queueInputBuffer(decoder->codec, (size_t)input_index,
                                                                    0, 0, 0,
                                                                    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    DBG("queue EOS -> %d", (int)q);
                    decoder->input_eos = 1;
                } else {
                    int64_t pts = AMediaExtractor_getSampleTime(decoder->extractor);
                    media_status_t q = AMediaCodec_queueInputBuffer(decoder->codec, (size_t)input_index,
                                                                    0, (size_t)read,
                                                                    (uint64_t)(pts < 0 ? 0 : pts), 0);
                    (void)q;
                    AMediaExtractor_advance(decoder->extractor);
                }
            }
        }

        AMediaCodecBufferInfo info;
        ssize_t output_index = AMediaCodec_dequeueOutputBuffer(decoder->codec, &info, DEQUEUE_TIMEOUT_US);
        if (loop < 60)
            DBG("  dequeueOutputBuffer -> %d size=%d flags=%u", (int)output_index, (int)info.size, info.flags);

        if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            const char *out_string;
            apply_output_format(decoder);
            {
                AMediaFormat *output_format = AMediaCodec_getOutputFormat(decoder->codec);
                out_string = AMediaFormat_toString(output_format);
                DBG("output format changed: %s", out_string != NULL ? out_string : "(null)");
                if (output_format != NULL)
                    AMediaFormat_delete(output_format);
            }
            continue;
        }
        if (output_index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        }
        if (output_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            /* Once the input is at EOS the decoder drains its remaining frames
             * before reporting it; give it a bounded amount of time. */
            if (decoder->input_eos && ++idle > MAX_IDLE_LOOPS / 2)
                return 0;
            continue;
        }
        if (output_index < 0) {
            LOGE("dequeueOutputBuffer failed: %d", (int)output_index);
            return 0;
        }

        if (info.size > 0) {
            size_t capacity = 0;
            uint8_t *output = AMediaCodec_getOutputBuffer(decoder->codec, (size_t)output_index, &capacity);
            int ok = 0;
            if (output != NULL && (size_t)info.offset + (size_t)info.size <= capacity)
                ok = convert_output(decoder, output + info.offset, (size_t)info.size);
            AMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)output_index, false);
            if (ok)
                return 1;
            continue;
        }

        AMediaCodec_releaseOutputBuffer(decoder->codec, (size_t)output_index, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
            decoder->output_eos = 1;
            return 0;
        }
    }

    LOGW("decoder stalled while draining");
    return 0;
}

media_decoder *media_decoder_open(const char *path)
{
    media_decoder *decoder;
    media_decoder_log("open request: %s", path);
    decoder = (media_decoder *)calloc(1, sizeof(*decoder));
    if (decoder == NULL)
        return NULL;

    decoder->extractor = AMediaExtractor_new();
    if (decoder->extractor == NULL) {
        free(decoder);
        return NULL;
    }

    {
        media_status_t st = set_data_source(decoder->extractor, path);
        media_decoder_log("data source -> %d", (int)st);
        if (st != AMEDIA_OK) {
            LOGW("not a container the platform can demux: %s", path);
            goto fail;
        }
    }

    size_t track_count = AMediaExtractor_getTrackCount(decoder->extractor);
    const char *mime = NULL;
    for (size_t i = 0; i < track_count; i++) {
        AMediaFormat *format = AMediaExtractor_getTrackFormat(decoder->extractor, i);
        if (format == NULL)
            continue;
        const char *format_mime = NULL;
        if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &format_mime) &&
            format_mime != NULL && strncmp(format_mime, "audio/", 6) == 0) {
            decoder->track_format = format;
            mime = format_mime;
            AMediaExtractor_selectTrack(decoder->extractor, i);
            break;
        }
        AMediaFormat_delete(format);
    }

    if (decoder->track_format == NULL || mime == NULL) {
        LOGW("no audio track in %s", path);
        goto fail;
    }

    decoder->rate_in = format_int32(decoder->track_format, AMEDIAFORMAT_KEY_SAMPLE_RATE, 0);
    decoder->channels_in = format_int32(decoder->track_format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 0);

    int64_t duration = 0;
    if (AMediaFormat_getInt64(decoder->track_format, AMEDIAFORMAT_KEY_DURATION, &duration))
        decoder->duration_us = duration;

    char *mime_copy = strdup(mime);
    if (mime_copy == NULL)
        goto fail;

    DBG("audio track: index-mime=%s rate=%d channels=%d duration=%lld",
        mime_copy, decoder->rate_in, decoder->channels_in, (long long)decoder->duration_us);
    {
        char *format_string = AMediaFormat_toString(decoder->track_format);
        media_decoder_log("track: mime=%s rate=%d channels=%d duration_us=%lld format=%s",
                          mime_copy, decoder->rate_in, decoder->channels_in,
                          (long long)decoder->duration_us,
                          format_string != NULL ? format_string : "(null)");
        /* NOTE: AMediaFormat_toString() returns storage owned by the format;
         * it must NOT be free()d. */
    }
    decoder->codec = AMediaCodec_createDecoderByType(mime_copy);
    media_decoder_log("createDecoderByType(%s) -> %p", mime_copy, (void *)decoder->codec);
    if (decoder->codec == NULL) {
        LOGW("no platform decoder for %s", mime_copy);
        free(mime_copy);
        goto fail;
    }

    {
        media_status_t st = AMediaCodec_configure(decoder->codec, decoder->track_format, NULL, NULL, 0);
        media_decoder_log("configure -> %d", (int)st);
        if (st != AMEDIA_OK) {
            LOGE("codec configure failed for %s", mime_copy);
            free(mime_copy);
            goto fail;
        }
    }
    free(mime_copy);

    {
        media_status_t st = AMediaCodec_start(decoder->codec);
        media_decoder_log("start -> %d", (int)st);
        if (st != AMEDIA_OK) {
            LOGE("codec start failed");
            goto fail;
        }
    }

    media_decoder_log("opened %s: mime=%s rate_in=%d channels_in=%d duration=%.3fs",
                     path, mime, decoder->rate_in, decoder->channels_in,
                     (double)decoder->duration_us / 1000000.0);

    decoder->rate_out = decoder->rate_in;
    decoder->channels_out = decoder->channels_in;
    decoder->pcm_encoding = PCM_ENCODING_16BIT;

    /* Prime the decoder: learn the real output format and have the first
     * samples ready for getRate()/readSamples().  An empty stream is fine. */
    {
        int primed = refill(decoder);
        DBG("priming refill -> %d (rate_out=%d channels_out=%d encoding=%d samples=%d)",
            primed, decoder->rate_out, decoder->channels_out, decoder->pcm_encoding, decoder->pcm_samples);
    }
    if (decoder->rate_out <= 0)
        decoder->rate_out = decoder->rate_in;

    return decoder;

fail:
    media_decoder_log("open failed: %s", path);
    media_decoder_close(decoder);
    return NULL;
}

int media_decoder_read(media_decoder *decoder, int16_t *dst, int count)
{
    if (decoder == NULL || dst == NULL || count <= 0)
        return 0;

    int copied = 0;
    while (copied < count) {
        if (decoder->pcm_pos >= decoder->pcm_samples) {
            if (!refill(decoder))
                return 0;
        }
        int available = decoder->pcm_samples - decoder->pcm_pos;
        int chunk = count - copied;
        if (chunk > available)
            chunk = available;
        memcpy(dst + copied, decoder->pcm + decoder->pcm_pos, (size_t)chunk * sizeof(int16_t));
        decoder->pcm_pos += chunk;
        copied += chunk;
    }
    return copied;
}

int media_decoder_skip(media_decoder *decoder, int count)
{
    if (decoder == NULL || count <= 0)
        return 0;

    int skipped = 0;
    while (skipped < count) {
        if (decoder->pcm_pos >= decoder->pcm_samples) {
            if (!refill(decoder))
                return 0;
        }
        int available = decoder->pcm_samples - decoder->pcm_pos;
        int chunk = count - skipped;
        if (chunk > available)
            chunk = available;
        decoder->pcm_pos += chunk;
        skipped += chunk;
    }
    return skipped;
}

int media_decoder_seek(media_decoder *decoder, int frame)
{
    if (decoder == NULL || decoder->extractor == NULL || frame < 0)
        return -1;

    int rate = decoder->rate_out > 0 ? decoder->rate_out : decoder->rate_in;
    if (rate <= 0)
        return -1;

    int64_t time_us = (int64_t)frame * 1000000 / rate;
    if (AMediaExtractor_seekTo(decoder->extractor, time_us, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC) != AMEDIA_OK) {
        LOGW("extractor seek to %d frames failed", frame);
        return -1;
    }
    if (AMediaCodec_flush(decoder->codec) != AMEDIA_OK) {
        LOGW("codec flush failed");
        return -1;
    }

    decoder->pcm_pos = 0;
    decoder->pcm_samples = 0;
    decoder->input_eos = 0;
    decoder->output_eos = 0;
    return frame;
}

static int64_t total_frames(media_decoder *decoder)
{
    int rate = decoder->rate_out > 0 ? decoder->rate_out : decoder->rate_in;
    if (rate <= 0 || decoder->duration_us <= 0)
        return -1;
    return decoder->duration_us * (int64_t)rate / 1000000;
}

int media_decoder_seek_end(media_decoder *decoder, int delta_frames)
{
    int64_t frames = total_frames(decoder);
    if (frames < 0)
        return -1;
    int64_t target = frames + delta_frames;
    if (target < 0 || target > frames)
        return -1;
    return media_decoder_seek(decoder, (int)target);
}

int media_decoder_rate(const media_decoder *decoder)
{
    if (decoder == NULL)
        return 0;
    return decoder->rate_out > 0 ? decoder->rate_out : decoder->rate_in;
}

int media_decoder_channels(const media_decoder *decoder)
{
    (void)decoder;
    return 2;
}

float media_decoder_length(const media_decoder *decoder)
{
    if (decoder == NULL || decoder->duration_us <= 0)
        return 0.0f;
    return (float)((double)decoder->duration_us / 1000000.0);
}

void media_decoder_close(media_decoder *decoder)
{
    if (decoder == NULL)
        return;
    if (decoder->codec != NULL) {
        AMediaCodec_stop(decoder->codec);
        AMediaCodec_delete(decoder->codec);
        decoder->codec = NULL;
    }
    if (decoder->extractor != NULL) {
        AMediaExtractor_delete(decoder->extractor);
        decoder->extractor = NULL;
    }
    if (decoder->track_format != NULL) {
        AMediaFormat_delete(decoder->track_format);
        decoder->track_format = NULL;
    }
    free(decoder->pcm);
    free(decoder);
}
