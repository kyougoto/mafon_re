/*
 * media_test.c -- on-device smoke test for media_decoder.c.
 *
 * Built for the device ABI, pushed to /data/local/tmp and executed via adb
 * shell.  It decodes a file with the exact same code that libmpg.so uses and
 * prints the results to stdout (useful on ROMs where adb logcat is disabled).
 *
 * Linux/Android caveat: on Android 15+ the NDK MediaCodec APIs do not deliver
 * output buffers to a process that has no JavaVM, so this shell executable is
 * only usable on Android 14 and below.  Inside the Mafon app process (which has
 * a JVM) the very same media_decoder.c works normally.
 *
 * Build (arm64):
 *   clang --target=aarch64-linux-android21 \
 *         --sysroot=<NDK>/toolchains/llvm/prebuilt/windows-x86_64/sysroot \
 *         -O2 -o media_test media_test.c media_decoder.c -lmediandk -llog
 */

#include "media_decoder.h"

#include <stdio.h>
#include <stdint.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <audio-file>\n", argv[0]);
        return 2;
    }

    media_decoder *decoder = media_decoder_open(argv[1]);
    if (decoder == NULL) {
        printf("OPEN FAILED: %s\n", argv[1]);
        return 1;
    }

    printf("OPEN OK rate=%d channels=%d length=%.3f\n",
           media_decoder_rate(decoder),
           media_decoder_channels(decoder),
           (double)media_decoder_length(decoder));

    static int16_t buffer[16384];
    long total = 0;
    long nonzero = 0;
    int call = 0;

    for (; call < 4000; call++) {
        int n = media_decoder_read(decoder, buffer, 16384);
        if (n <= 0)
            break;
        total += n;
        for (int i = 0; i < n; i++) {
            if (buffer[i] != 0)
                nonzero++;
        }
    }

    printf("DECODED calls=%d samples=%ld nonzero=%ld\n", call, total, nonzero);

    int seeked = media_decoder_seek(decoder, 44100);
    printf("SEEK(44100 frames) -> %d\n", seeked);
    int after = media_decoder_read(decoder, buffer, 16384);
    printf("AFTER SEEK read=%d first=%d\n", after, after > 0 ? buffer[0] : 0);

    media_decoder_close(decoder);
    return total > 0 ? 0 : 1;
}
