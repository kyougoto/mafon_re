# native/jni — MP3Decoder JNI wrapper (`libmpg.so`)

`mpg_jni.c` is the **reverse-engineered** JNI bridge that implements
`nobleworks.libmpg.MP3Decoder` on top of mpg123.  The original application only
shipped a 32-bit (`armeabi`/`armeabi-v7a`) `libmpg.so` without source, so this
file was reconstructed from the ARM disassembly; see
[`docs/ANALYSIS.md`](../../docs/ANALYSIS.md) for the full derivation
(`Decoder` struct layout, `openFile` format negotiation, `readSamples` buffer
refill, seek semantics, encoding-ordinal mapping).

## Multiple-format support (`media_decoder.c`)

The original `libmpg.so` is mpg123-only (MP3), while the app lists every audio file from
MediaStore.  `media_decoder.c/.h` adds a fallback decoder built on NDK
`AMediaExtractor` + `AMediaCodec` (libmediandk, API 21+):

* `mpg_jni.c` first tries the original mpg123 path (`open_mpg123()`), so MP3 behaviour is unchanged;
* if that fails it opens the file with `media_decoder_open()`, which demuxes/decodes any audio the
  platform supports (m4a/AAC, flac, ogg/vorbis, opus, wav, ...) into interleaved 16-bit stereo PCM
  at the stream's native rate and serves it through the same `readSamples/skipSamples/seek*` API.

`media_test.c` is a small on-device smoke test for the decoder.  Note: on Android 15+ the NDK
MediaCodec APIs do not produce output in a plain native process without a JavaVM, so `media_test.c`
is only meaningful on Android 14 and below (or when packaged into an app); inside the Mafon app
process it works normally.

## Build wiring

| property | value |
| --- | --- |
| CMake target | `mpg` (SHARED) |
| output | `libmpg.so` |
| source | `jni/mpg_jni.c` when it exists, otherwise `jni/mpg_jni_placeholder.c` |
| include dirs | `native/jni`, `src/mpg123/src/include` (so `#include <mpg123.h>` works) |
| links | static `libmpg123.a` (mpg123 1.32.10) + `-llog -lm` |
| staged to | `native/out/<abi>/libmpg.so` (real wrapper only; placeholder is never staged) |

If `mpg_jni.c` is absent the project still configures, but `libmpg.so` is only
built from the placeholder for validation and is not installed.  Pass
`-BuildMpg` to `build.ps1` to build the placeholder explicitly.

## Exported symbols

The wrapper must export exactly the JNI entry points that the application's
`classes.dex` binds to (same names the original 32-bit library exported):

```
Java_nobleworks_libmpg_MP3Decoder_initialize
Java_nobleworks_libmpg_MP3Decoder_getSupportedRates
Java_nobleworks_libmpg_MP3Decoder_getEncodings
Java_nobleworks_libmpg_MP3Decoder_getErrorMessage
Java_nobleworks_libmpg_MP3Decoder_00024Feature_isFeatureSupported
Java_nobleworks_libmpg_MP3Decoder_openFile
Java_nobleworks_libmpg_MP3Decoder_delete
Java_nobleworks_libmpg_MP3Decoder_readSamples
Java_nobleworks_libmpg_MP3Decoder_skipSamples
Java_nobleworks_libmpg_MP3Decoder_seekTo
Java_nobleworks_libmpg_MP3Decoder_seekToEnd
Java_nobleworks_libmpg_MP3Decoder_getNumChannels
Java_nobleworks_libmpg_MP3Decoder_getRate
Java_nobleworks_libmpg_MP3Decoder_getLength
```

`MP3Decoder.getFlags()` / `setFlags()` are declared in the app's dex as native
methods, but the original `.so` never exported them (they are dead library API
and are never called by the app), so this wrapper intentionally does not export
them either.

## Java side (as found in the app)

```java
package nobleworks.libmpg;

public class MP3Decoder {
    static { System.loadLibrary("mpg"); initialize(); }
    private long handle;
    public MP3Decoder(String filename);           // openFile()
    public int readSamples(java.nio.ShortBuffer); // direct buffer, 16-bit PCM
    public int skipSamples(int);
    public int seekTo(int);  public int seekToEnd(int);
    public int getNumChannels(); public int getRate(); public float getLength();
    public void dispose();
}
```
