/*
 * mafon_mpg_jni.c -- JNI wrapper for libmpg (mpg123) used by Mafon.
 *
 * This is a clean-room reimplementation for arm64-v8a of the original
 * 32-bit-only libmpg.so that ships in com.neosphere.mafon.  The original
 * library was a thin JNI bridge around mpg123 whose Java companion class is
 * nobleworks.libmpg.MP3Decoder.  The public JNI surface (class names,
 * method names and signatures) must stay byte-for-byte identical to what
 * the app's classes.dex expects; the behaviour below was reconstructed from
 * the original libmpg.so (armeabi-v7a) disassembly:
 *
 *   Decoder layout (32 bytes in the original):
 *     0x00  mpg123_handle *handle
 *     0x04  int   channels
 *     0x08  int   rate
 *     0x0c  float length        (seconds)
 *     0x10  int   outblock      (bytes of the decode buffer)
 *     0x14  void *buffer
 *     0x18  int   samples_available  (16-bit samples left in buffer)
 *     0x1c  int   buffer_pos         (index into buffer, 16-bit samples)
 *
 *   openFile() opens the stream, forces MPG123_ENC_SIGNED_16 output for the
 *   stream's native rate/channels, allocates the outblock buffer and caches
 *   the track length; readSamples()/skipSamples() drain that buffer and
 *   refill it with mpg123_read() on demand; seekTo()/seekToEnd() forward to
 *   mpg123_seek() with SEEK_SET / SEEK_END.
 *
 * License: the app's original libmpg is a part of the Mafon player; this
 * reimplementation is provided for interoperability with the unmodified
 * app binary.  mpg123 itself is LGPL-2.1.
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <mpg123.h>

#include "media_decoder.h"

typedef struct mafon_decoder {
    int mode; /* 0 = mpg123 (MP3), 1 = Android MediaCodec fallback */
    mpg123_handle *handle;
    int channels;
    int rate;
    float length;
    size_t outblock;
    unsigned char *buffer;
    int samples_available; /* in 16-bit samples */
    int buffer_pos;        /* in 16-bit samples */
    media_decoder *media;  /* mode 1 only */
} mafon_decoder;

static mafon_decoder *as_decoder(jlong handle)
{
    return (mafon_decoder *)(intptr_t)handle;
}

/* Map an mpg123 encoding constant to the ordinal used by
 * nobleworks.libmpg.Encoding (declaration order of the Java enum):
 *   0 ULAW_8, 1 ALAW_8, 2 PCM_SIGNED_8, 3 PCM_UNSIGNED_8,
 *   4 PCM_SIGNED_16, 5 PCM_UNSIGNED_16, 6 PCM_SIGNED_24,
 *   7 PCM_UNSIGNED_24, 8 PCM_SIGNED_32, 9 PCM_UNSIGNED_32,
 *   10 PCM_FLOAT_32, 11 PCM_FLOAT_64
 * The original implementation hard-coded the (then current) numeric
 * constants; we use the symbolic names so the mapping survives mpg123
 * releases that renumber the encoding bits.
 */
static jint encoding_ordinal(int encoding)
{
    switch (encoding) {
    case MPG123_ENC_ULAW_8:      return 0;
    case MPG123_ENC_ALAW_8:      return 1;
    case MPG123_ENC_SIGNED_8:    return 2;
    case MPG123_ENC_UNSIGNED_8:  return 3;
    case MPG123_ENC_SIGNED_16:   return 4;
    case MPG123_ENC_UNSIGNED_16: return 5;
    case MPG123_ENC_SIGNED_24:   return 6;
    case MPG123_ENC_UNSIGNED_24: return 7;
    case MPG123_ENC_SIGNED_32:   return 8;
    case MPG123_ENC_UNSIGNED_32: return 9;
    case MPG123_ENC_FLOAT_32:    return 10;
    case MPG123_ENC_FLOAT_64:    return 11;
    default:                     return -1;
    }
}

JNIEXPORT jint JNICALL
Java_nobleworks_libmpg_MP3Decoder_initialize(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    return (jint)mpg123_init();
}

JNIEXPORT jintArray JNICALL
Java_nobleworks_libmpg_MP3Decoder_getSupportedRates(JNIEnv *env, jclass clazz)
{
    const long *rates = NULL;
    size_t count = 0;
    jintArray result;
    jint *values;

    (void)clazz;
    mpg123_rates(&rates, &count);
    if (count == 0 || rates == NULL)
        return NULL;

    result = (*env)->NewIntArray(env, (jsize)count);
    if (result == NULL)
        return NULL;

    values = (*env)->GetIntArrayElements(env, result, NULL);
    if (values == NULL) {
        (*env)->DeleteLocalRef(env, result);
        return NULL;
    }

    {
        size_t i;
        for (i = 0; i < count; i++)
            values[i] = (jint)rates[i];
    }

    (*env)->ReleaseIntArrayElements(env, result, values, 0);
    return result;
}

JNIEXPORT jintArray JNICALL
Java_nobleworks_libmpg_MP3Decoder_getEncodings(JNIEnv *env, jclass clazz)
{
    const int *encodings = NULL;
    size_t count = 0;
    jintArray result;
    jint *values;

    (void)clazz;
    mpg123_encodings(&encodings, &count);
    if (count == 0 || encodings == NULL)
        return NULL;

    result = (*env)->NewIntArray(env, (jsize)count);
    if (result == NULL)
        return NULL;

    values = (*env)->GetIntArrayElements(env, result, NULL);
    if (values == NULL) {
        (*env)->DeleteLocalRef(env, result);
        return NULL;
    }

    {
        size_t i;
        for (i = 0; i < count; i++)
            values[i] = encoding_ordinal(encodings[i]);
    }

    (*env)->ReleaseIntArrayElements(env, result, values, 0);
    return result;
}

JNIEXPORT jstring JNICALL
Java_nobleworks_libmpg_MP3Decoder_getErrorMessage(JNIEnv *env, jclass clazz, jint error)
{
    const char *message;

    (void)clazz;
    message = mpg123_plain_strerror((int)error);
    if (message == NULL)
        message = "";
    return (*env)->NewStringUTF(env, message);
}

JNIEXPORT jboolean JNICALL
Java_nobleworks_libmpg_MP3Decoder_00024Feature_isFeatureSupported(JNIEnv *env, jclass clazz, jint feature)
{
    (void)env;
    (void)clazz;
    return (jboolean)(mpg123_feature((int)feature) ? JNI_TRUE : JNI_FALSE);
}

/*
 * Original MP3 path: open the file with mpg123 and force signed 16-bit PCM
 * output at the stream's native rate/channels.  Returns NULL when mpg123
 * cannot decode the file (e.g. m4a/AAC, flac, ogg, ...).
 */
static mafon_decoder *open_mpg123(JNIEnv *env, jstring filename)
{
    mafon_decoder *decoder;
    mpg123_handle *handle;
    const char *path;
    int error = MPG123_OK;
    int encoding = 0;
    long rate = 0;
    off_t length;

    handle = mpg123_new(NULL, &error);
    if (handle == NULL || error != MPG123_OK)
        return NULL;

    decoder = (mafon_decoder *)calloc(1, sizeof(*decoder));
    if (decoder == NULL) {
        mpg123_delete(handle);
        return NULL;
    }
    decoder->mode = 0;
    decoder->handle = handle;

    path = (*env)->GetStringUTFChars(env, filename, NULL);
    if (path == NULL) {
        mpg123_delete(handle);
        free(decoder);
        return NULL;
    }
    error = mpg123_open(handle, path);
    (*env)->ReleaseStringUTFChars(env, filename, path);
    if (error != MPG123_OK)
        goto fail;

    error = mpg123_getformat(handle, &rate, &decoder->channels, &encoding);
    if (error != MPG123_OK || encoding != MPG123_ENC_SIGNED_16)
        goto fail;
    decoder->rate = (int)rate;

    /* Decode everything to signed 16-bit PCM, no resampling. */
    mpg123_format_none(handle);
    mpg123_format(handle, (long)decoder->rate, decoder->channels, MPG123_ENC_SIGNED_16);

    decoder->outblock = (size_t)mpg123_outblock(handle);
    if (decoder->outblock == 0)
        decoder->outblock = 1152 * 2 * (decoder->channels > 0 ? decoder->channels : 2);
    decoder->buffer = (unsigned char *)malloc(decoder->outblock);
    if (decoder->buffer == NULL)
        goto fail;
    decoder->samples_available = 0;
    decoder->buffer_pos = 0;

    length = mpg123_length(handle);
    if (length < 0 || decoder->rate <= 0)
        decoder->length = 0.0f;
    else
        decoder->length = (float)((double)length / (double)decoder->rate);

    return decoder;

fail:
    mpg123_close(handle);
    mpg123_delete(handle);
    free(decoder->buffer);
    free(decoder);
    return NULL;
}

/*
 * Open a file: try mpg123 first (MP3, the original behaviour), then fall back
 * to the platform decoder (MediaExtractor + MediaCodec) which handles m4a/AAC,
 * flac, ogg/vorbis, opus, wav, ...  Returns 0 if neither can decode it, which
 * the Java layer reports as "couldn't open file".
 */
JNIEXPORT jlong JNICALL
Java_nobleworks_libmpg_MP3Decoder_openFile(JNIEnv *env, jobject thiz, jstring filename)
{
    mafon_decoder *decoder;
    media_decoder *media;
    const char *path;

    (void)thiz;

    decoder = open_mpg123(env, filename);
    if (decoder != NULL)
        return (jlong)(intptr_t)decoder;

    path = (*env)->GetStringUTFChars(env, filename, NULL);
    if (path == NULL)
        return 0;
    media = media_decoder_open(path);
    (*env)->ReleaseStringUTFChars(env, filename, path);
    if (media == NULL)
        return 0;

    decoder = (mafon_decoder *)calloc(1, sizeof(*decoder));
    if (decoder == NULL) {
        media_decoder_close(media);
        return 0;
    }
    decoder->mode = 1;
    decoder->media = media;
    decoder->rate = media_decoder_rate(media);
    decoder->channels = media_decoder_channels(media);
    decoder->length = media_decoder_length(media);
    return (jlong)(intptr_t)decoder;
}

JNIEXPORT void JNICALL
Java_nobleworks_libmpg_MP3Decoder_delete(JNIEnv *env, jobject thiz, jlong handle)
{
    mafon_decoder *decoder = as_decoder(handle);

    (void)env;
    (void)thiz;
    if (decoder == NULL)
        return;

    if (decoder->mode == 1) {
        media_decoder_close(decoder->media);
    } else {
        mpg123_close(decoder->handle);
        mpg123_delete(decoder->handle);
        free(decoder->buffer);
    }
    free(decoder);
}

static int refill_mpg(mafon_decoder *decoder)
{
    size_t done = 0;
    int result = mpg123_read(decoder->handle, decoder->buffer, decoder->outblock, &done);

    decoder->buffer_pos = 0;
    decoder->samples_available = (int)(done / 2);
    if (result != MPG123_OK || done == 0)
        return 0;
    return 1;
}

/*
 * Copy up to `count` 16-bit samples from the decoder buffer into the direct
 * ShortBuffer and return the number of samples actually copied (0 at EOF).
 */
JNIEXPORT jint JNICALL
Java_nobleworks_libmpg_MP3Decoder_readSamples(JNIEnv *env, jobject thiz, jlong handle,
                                              jobject samples, jint count)
{
    mafon_decoder *decoder = as_decoder(handle);
    int16_t *dst;
    int copied = 0;

    (void)thiz;

    if (decoder == NULL || samples == NULL || count <= 0)
        return 0;

    dst = (int16_t *)(*env)->GetDirectBufferAddress(env, samples);
    if (dst == NULL)
        return 0;

    if (decoder->mode == 1)
        return (jint)media_decoder_read(decoder->media, dst, (int)count);

    while (copied < count) {
        int chunk;

        if (decoder->samples_available <= 0) {
            if (!refill_mpg(decoder))
                return 0; /* faithful to the original: EOF/error => 0 */
        }

        chunk = decoder->samples_available;
        if (chunk > count - copied)
            chunk = count - copied;

        memcpy(dst + copied,
               (const int16_t *)decoder->buffer + decoder->buffer_pos,
               (size_t)chunk * sizeof(int16_t));

        decoder->buffer_pos += chunk;
        decoder->samples_available -= chunk;
        copied += chunk;
    }

    return (jint)copied;
}

JNIEXPORT jint JNICALL
Java_nobleworks_libmpg_MP3Decoder_skipSamples(JNIEnv *env, jobject thiz, jlong handle,
                                              jint count)
{
    mafon_decoder *decoder = as_decoder(handle);
    int skipped = 0;

    (void)env;
    (void)thiz;

    if (decoder == NULL || count <= 0)
        return 0;

    if (decoder->mode == 1)
        return (jint)media_decoder_skip(decoder->media, (int)count);

    while (skipped < count) {
        int chunk;

        if (decoder->samples_available <= 0) {
            if (!refill_mpg(decoder))
                return 0;
        }

        chunk = decoder->samples_available;
        if (chunk > count - skipped)
            chunk = count - skipped;

        decoder->buffer_pos += chunk;
        decoder->samples_available -= chunk;
        skipped += chunk;
    }

    return (jint)skipped;
}

JNIEXPORT jint JNICALL
Java_nobleworks_libmpg_MP3Decoder_seekTo(JNIEnv *env, jobject thiz, jlong handle, jint sample)
{
    mafon_decoder *decoder = as_decoder(handle);

    (void)env;
    (void)thiz;
    if (decoder == NULL)
        return -1;

    if (decoder->mode == 1)
        return (jint)media_decoder_seek(decoder->media, (int)sample);

    /* A seek invalidates samples that were already buffered. */
    decoder->samples_available = 0;
    decoder->buffer_pos = 0;
    return (jint)mpg123_seek(decoder->handle, (off_t)sample, SEEK_SET);
}

JNIEXPORT jint JNICALL
Java_nobleworks_libmpg_MP3Decoder_seekToEnd(JNIEnv *env, jobject thiz, jlong handle, jint sample)
{
    mafon_decoder *decoder = as_decoder(handle);

    (void)env;
    (void)thiz;
    if (decoder == NULL)
        return -1;

    if (decoder->mode == 1)
        return (jint)media_decoder_seek_end(decoder->media, (int)sample);

    decoder->samples_available = 0;
    decoder->buffer_pos = 0;
    return (jint)mpg123_seek(decoder->handle, (off_t)sample, SEEK_END);
}

JNIEXPORT jint JNICALL
Java_nobleworks_libmpg_MP3Decoder_getNumChannels(JNIEnv *env, jobject thiz, jlong handle)
{
    mafon_decoder *decoder = as_decoder(handle);

    (void)env;
    (void)thiz;
    if (decoder == NULL)
        return 0;
    if (decoder->mode == 1)
        return (jint)media_decoder_channels(decoder->media);
    return (jint)decoder->channels;
}

JNIEXPORT jint JNICALL
Java_nobleworks_libmpg_MP3Decoder_getRate(JNIEnv *env, jobject thiz, jlong handle)
{
    mafon_decoder *decoder = as_decoder(handle);

    (void)env;
    (void)thiz;
    if (decoder == NULL)
        return 0;
    if (decoder->mode == 1)
        return (jint)media_decoder_rate(decoder->media);
    return (jint)decoder->rate;
}

JNIEXPORT jfloat JNICALL
Java_nobleworks_libmpg_MP3Decoder_getLength(JNIEnv *env, jobject thiz, jlong handle)
{
    mafon_decoder *decoder = as_decoder(handle);

    (void)env;
    (void)thiz;
    if (decoder == NULL)
        return 0.0f;
    if (decoder->mode == 1)
        return (jfloat)media_decoder_length(decoder->media);
    return (jfloat)decoder->length;
}
