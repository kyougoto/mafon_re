# mafon native libraries — Android build (BUILD.md)

Cross-built native libraries for the mafon APK on the **arm64-v8a** ABI
(plus an armeabi-v7a `libsonic.so` reference build and an x86_64 emulator
build), using the **Android NDK r26b** clang toolchain and plain C.

All shared libraries are built **16 KB-page compatible**
(`-Wl,-z,max-page-size=16384`, LOAD `p_align 0x4000`) for Android 15/16
devices with 16 KB memory pages — see §6.

| Artifact | ABI | Type | Purpose |
|---|---|---|---|
| `out\arm64-v8a\libmpg123.a` | arm64-v8a, API 21 | static archive, `-fPIC` | mpg123 1.32.10 core, linked into `libmpg.so` |
| `out\arm64-v8a\libsonic.so` | arm64-v8a, API 21 | shared, ELF64 AArch64, 16 KB pages | `org.vinuxproject.sonic.Sonic` JNI wrapper |
| `out\arm64-v8a\libmpg.so` | arm64-v8a, API 21 | shared, ELF64 AArch64, 16 KB pages | `nobleworks.libmpg.MP3Decoder` JNI wrapper (built when `jni\mpg_jni.c` exists) |
| `out\arm64-v8a\include\{mpg123.h,fmt123.h,sonic.h}` | — | headers | compile the JNI wrappers / app code |
| `out\armeabi-v7a\libsonic.so` + headers | armeabi-v7a, API 16 | shared, ELF32 ARM | reference/comparison build only |
| `out\x86_64\libmpg123.a`, `out\x86_64\libsonic.so`, `out\x86_64\libmpg.so` + headers | x86_64, API 21 | ELF64 x86-64, 16 KB pages | optional emulator smoke test |

Staged artifacts of the current build (unstripped, Release; sizes/hashes are
informational — rebuild may change build-ids):

| File | Bytes | SHA-256 |
|---|---|---|
| `out\arm64-v8a\libmpg123.a` | 1,857,662 | `26068C0AD6537B6A4F1D727488DFEED454C4FC263FC4F848598B75B7A8B753A5` |
| `out\arm64-v8a\libsonic.so` | 112,024 | `79FB8BC1D242D2D95C6071571EA78007319CA80A970C84F7B2092684DD95B643` |
| `out\arm64-v8a\libmpg.so` | 1,126,976 | `A18D5DFB91410F583040FA959B499B52919479EB1FA70DF8D3F5648E509F17E8` |
| `out\armeabi-v7a\libsonic.so` | 86,580 | `C35E6008835747D8683A446735D683209883962AE5276A6858AD694F72E0CDD7` |
| `out\x86_64\libmpg123.a` | 1,768,014 | `0FA0265C93A64AE1609651136FFDAD84BBA2BC5D92ED9F50D838AB2C233366C7` |
| `out\x86_64\libsonic.so` | 110,888 | `6B546D78011F10CB339B4082445209DA16C682F3903F4DB6CC7506113F304090` |
| `out\x86_64\libmpg.so` | 1,075,256 | `BB3940BA4E8064D44A07B8B13A0819797B0F7ED2E529DAADE4025883339BE528` |

Headers: `mpg123.h` 102,425 B (`C47E918A…`), `fmt123.h` 6,089 B (`9B7C2567…`),
`sonic.h` 7,303 B (`534C7F14…`).

---

## 1. Prerequisites

* **Android NDK r26b** (provides clang 17, `llvm-readelf`, `llvm-nm`,
  `llvm-objdump`, and `build\cmake\android.toolchain.cmake`).
  Resolution order for `-Ndk`: parameter → `$env:ANDROID_NDK_ROOT` →
  `$env:ANDROID_NDK_HOME` → `%ANDROID_NDK_ROOT%`
  (change the last fallback for a different checkout/CI).
* **CMake ≥ 3.20 and Ninja**. On this host they are pip-installed:

  ```bat
  python -m pip install cmake ninja
  ```

  → `cmake` (4.4.4) and `ninja`.
  `build.ps1` finds `cmake`/`ninja` in this order: `-CMake`/`-Ninja` parameter →
  `PATH` → directory next to `-Python` (`$env:MAFON_PYTHON`, else
  `python`) → `<python>\Scripts` as last-resort hint.
* Sources under `..\src` (sibling of `native\`):
  * `src\mpg123` — mpg123 1.32.10 (official tarball)
  * `src\sonic`   — waywardgeek/sonic-ndk (`jni\sonic.c`, `sonic.h`,
    `sonicjni.c`, `sonicjni.h`)

  `CMakeLists.txt` computes `MAFON_SRC_ROOT` as `<this dir>\..\src`; override
  with `-DMAFON_SRC_ROOT=<path>` if the tree lives elsewhere.

### PowerShell execution policy

Some hosts (this one uses `CurrentUser = RemoteSigned`) reject unsigned local
scripts with `& .\build.ps1`. Use either the wrapper or an explicit bypass:

```bat
build.cmd -Abi arm64-v8a
:: or
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Abi arm64-v8a
```

(`build.cmd` is a 4-line wrapper around `build.ps1` with `-ExecutionPolicy Bypass`.)

---

## 2. Quick start

```bat
cd <repo>\native          :: <repo>\native in this workspace

build.cmd                          :: arm64-v8a + armeabi-v7a
build.cmd -Abi arm64-v8a           :: just the app ABI
build.cmd -Abi arm64-v8a -Clean    :: wipe build\<abi> and out\<abi> first
build.cmd -Abi x86_64              :: emulator ABI (libmpg123.a + libsonic.so + libmpg.so)
```

`build.ps1` parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `-Abi` | `arm64-v8a, armeabi-v7a` | ABIs to build (`x86_64`, `x86` also accepted) |
| `-Ndk` | env `ANDROID_NDK_ROOT`/`ANDROID_NDK_HOME`, else local r26b path | NDK root |
| `-Api` | `0` (=AUTO: arm64/x86_64→21, armv7/x86→16) | force an API level |
| `-BuildType` | `Release` | CMake build type |
| `-MaxPageSize` | `16384` | ELF LOAD alignment for shared libs (16 KB pages); `0` = linker default |
| `-Clean` | off | delete `build\<abi>` + `out\<abi>` first |
| `-SkipMpg` | off | never build the `libmpg.so` target |
| `-BuildMpg` | off | build the placeholder `libmpg.so` when `jni\mpg_jni.c` is absent (not staged) |
| `-CMake` / `-Ninja` / `-Python` | auto | explicit tool paths |
| `-NoVerify` | off | skip the built-in ELF/symbol/alignment checks |

Target selection: `libmpg123.a`/`libmpg.so` are built for the **64-bit ABIs**
(`arm64-v8a`, `x86_64`); `libsonic.so` for every ABI. `libmpg.so` is built only
when `jni\mpg_jni.c` exists (or `-BuildMpg` for the placeholder).
The script configures, builds, `cmake --install`s the artifacts into
`out\<abi>\`, runs the verification in §7 (including the 16 KB `p_align`
check), and exits non-zero on any failure.
Logs: `buildlogs\<abi>\build-{configure,compile,install}.log`.

---

## 3. Raw CMake commands (what build.ps1 runs)

arm64-v8a (the deliverable):

```bat
set NDK=%ANDROID_NDK_ROOT%          :: e.g. %ANDROID_NDK_ROOT%
set CMAKE=cmake
cd /d <repo>\native

%CMAKE% -G Ninja -S . -B build\arm64-v8a ^
  -DCMAKE_TOOLCHAIN_FILE="%NDK%\build\cmake\android.toolchain.cmake" ^
  -DANDROID_ABI=arm64-v8a ^
  -DANDROID_PLATFORM=android-21 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MAKE_PROGRAM=ninja ^
  -DCMAKE_INSTALL_PREFIX="%CD%\out\arm64-v8a" ^
  -DMAFON_BUILD_MPG123=ON ^
  -DMAFON_BUILD_SONIC=ON ^
  -DMAFON_BUILD_MPG=ON ^
  -DMAFON_MAX_PAGE_SIZE=16384

%CMAKE% --build build\arm64-v8a
%CMAKE% --install build\arm64-v8a
```

armeabi-v7a reference build: same, but
`-DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-16
-DMAFON_BUILD_MPG123=OFF -DMAFON_BUILD_MPG=OFF`.

x86_64 emulator build: same as arm64-v8a with `-DANDROID_ABI=x86_64`
(platform `android-21`).

> CMake 4.4.4 prints deprecation warnings from NDK r26b's
> `android.toolchain.cmake` (`cmake_minimum_required` 3.6 vs 3.10). They are
> harmless; the build returns 0.

---

## 4. Project layout

```
<repo>\
├─ src\                              (pristine upstream sources, unmodified)
│  ├─ mpg123\                        mpg123 1.32.10
│  │  └─ src\{libmpg123,compat,include}\...
│  └─ sonic\
│     └─ jni\{sonic.c,sonic.h,sonicjni.c,sonicjni.h,Android.mk,Application.mk}
└─ native\                           (this CMake project)
   ├─ CMakeLists.txt                 targets: mpg123static, sonic, mpg
   ├─ build.ps1                      one-command build + verification
   ├─ build.cmd                      execution-policy-safe launcher
   ├─ BUILD.md                       this file
   ├─ .gitignore                     ignores build\ buildlogs\ out\
   ├─ cmake\config-mpg123.h.in       generated config.h for mpg123 on Android
   ├─ jni\
   │  ├─ mpg_jni.c                   MP3Decoder JNI wrapper (real source, auto-detected)
   │  ├─ mpg_jni_placeholder.c       placeholder for the mpg target
   │  └─ README.md
   ├─ build\<abi>\                   CMake/Ninja build trees (regenerable, not committed)
   ├─ buildlogs\<abi>\               configure/compile/install logs (not committed)
   └─ out\<abi>\                     staged artifacts + include\ (not committed)
```

## 5. Targets and important flags

### `mpg123static` → `libmpg123.a`

* Sources: `src/mpg123/src/libmpg123/*.c` (parse, frame, format, dct64,
  equalizer, id3, optimize, readers, tabinit, libmpg123, index, icy, icy2utf8,
  layer1/2/3, ntom, synth, synth_8bit, synth_s32, synth_real, stringbuf,
  feature, lfs_wrap) + `src/compat/{compat,compat_str}.c`.
* **`CMAKE_POSITION_INDEPENDENT_CODE ON`** → every object is compiled with
  **`-fPIC`** (verified: `FLAGS = ... -std=gnu99 -fPIC -fvisibility=default`;
  29 `-fPIC` occurrences in the arm64 `build.ninja`). This is what allows the
  static archive to be linked into `libmpg.so`. Extra proof: the real-JNI
  build links the archive into `libmpg.so` with no
  `recompile with -fPIC` relocation errors.
* Compile definitions: **`-DOPT_GENERIC -DREAL_IS_FLOAT`** → portable generic C
  synth/dct, no NEON/SSE/assembly. `-fvisibility=default`.
* `config.h` is generated from `cmake/config-mpg123.h.in` into
  `build/<abi>/mpg123-config/config.h`:
  * `SIZEOF_OFF_T 8`, `IEEE_FLOAT`, `GAPLESS`, `FRAME_INDEX` (INDEX_SIZE 1000),
    `ACCURATE_ROUNDING`, `ASMALIGN_BALIGN`, `CCALIGN`, `USE_NEW_HUFFTABLE`,
    bionic `HAVE_*` headers.
  * no `PORTABLE_API`, so the standard off_t API (`mpg123_open`) is present;
    all layers and all output formats enabled; no network / no libout123 /
    no console programs / no runtime table generation.
  * `_FILE_OFFSET_BITS=64` is added only for 32-bit pointer targets; on
    arm64-v8a / x86_64 `off_t` is already 64-bit.
* Static archive is **not** stripped; it keeps 158 defined `mpg123_*` symbols
  (including the full decode API required by the JNI wrapper).

### `sonic` → `libsonic.so`

* Sources: `src/sonic/jni/sonic.c` + `src/sonic/jni/sonicjni.c` (the exact
  upstream JNI wrapper, package `org.vinuxproject.sonic`), target
  `sonic`, `-fPIC`, links `-llog -lm` (as upstream `Android.mk` does),
  linked with `-Wl,-z,max-page-size=16384`.
* Exports the 20 `Java_org_vinuxproject_sonic_Sonic_*` entry points and the
  public `sonic*` API.

### `mpg` → `libmpg.so`

* Source: `jni/mpg_jni.c` when present; otherwise `jni/mpg_jni_placeholder.c`
  (build-pipeline smoke test, **never staged**).
* Links `mpg123static` + `-llog -lm`; include path has `jni\` and
  `src\mpg123\src\include` so `#include <mpg123.h>` resolves.
* Linked with `-Wl,-z,max-page-size=16384`.
* Output name `mpg` (`libmpg.so`), staged into `out/<abi>/` **only** for the
  real wrapper.

---

## 6. 16 KB page size compatibility (Android 15/16)

Android devices configured with **16 KB memory pages** require shared-library
ELF `LOAD` segments aligned to 16 KB. NDK r26b's lld still links 4 KB
(`p_align 0x1000`) by default, so the build passes
**`-Wl,-z,max-page-size=16384`** to the `sonic` and `mpg` shared targets
(CMake: `MAFON_MAX_PAGE_SIZE`, default `16384`; `0` disables it). This is
applied to every shared library of every Android ABI (harmless on 4 KB-page
devices, required on 16 KB-page devices).

Verify for every `.so`:

```bat
llvm-readelf -lW out\arm64-v8a\libsonic.so
llvm-readelf -lW out\arm64-v8a\libmpg.so
```

Expected (all `LOAD` rows, not just the first):

```
  LOAD  0x000000 0x0000000000000000 ... R E 0x4000
  LOAD  ...       ...                  RW  0x4000
```

`build.ps1` checks this automatically and fails if any `LOAD` `p_align` is not
`0x4000` (`-MaxPageSize 0` disables both the flag and the check).

Current status (verified):

| ABI | libsonic.so | libmpg.so |
|---|---|---|
| arm64-v8a | LOAD p_align 0x4000 (2 LOAD) | LOAD p_align 0x4000 (3 LOAD) |
| x86_64 | LOAD p_align 0x4000 (2 LOAD) | LOAD p_align 0x4000 (3 LOAD) |

---

## 7. Verification (run automatically by build.ps1)

Tools from `%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin`.
Full captured output: `buildlogs\verification-16kb.txt` (arm64 + x86_64) and
`buildlogs\verification-final.txt` (pre-16 KB arm64 snapshot).

### libsonic.so (arm64-v8a) — actual output

```
Class:   ELF64
Data:    2's complement, little endian
Type:    DYN (Shared object file)
Machine: AArch64

NEEDED  liblog.so
NEEDED  libm.so
NEEDED  libdl.so
NEEDED  libc.so
SONAME  libsonic.so

LOAD    ... R E 0x4000
LOAD    ... RW  0x4000

Java_org_vinuxproject_sonic_Sonic_initNative
Java_org_vinuxproject_sonic_Sonic_closeNative
Java_org_vinuxproject_sonic_Sonic_putBytesNative
Java_org_vinuxproject_sonic_Sonic_receiveBytesNative
Java_org_vinuxproject_sonic_Sonic_setPitchNative      ... (20/20 present)
```

Commands:

```bat
llvm-readelf -h out\arm64-v8a\libsonic.so
llvm-readelf -d out\arm64-v8a\libsonic.so
llvm-readelf -lW out\arm64-v8a\libsonic.so
llvm-nm -D --defined-only out\arm64-v8a\libsonic.so | findstr Java_org_vinuxproject_sonic_Sonic_
```

### libmpg123.a (arm64-v8a) — actual output

* 26 archive members, every member `file format elf64-littleaarch64`,
  `architecture: aarch64` (`llvm-objdump -f`).
* Required API present (checked individually, 14/14):
  `mpg123_init, mpg123_new, mpg123_open, mpg123_getformat, mpg123_format_none,
  mpg123_format, mpg123_read, mpg123_seek, mpg123_length, mpg123_outblock,
  mpg123_plain_strerror, mpg123_feature, mpg123_rates, mpg123_encodings`
  — 158 `T mpg123_*` functions in total.

```bat
llvm-objdump -f out\arm64-v8a\libmpg123.a
llvm-nm --defined-only out\arm64-v8a\libmpg123.a | findstr " T mpg123_"
```

### libmpg.so (arm64-v8a) — actual output

```
Class: ELF64, Type: DYN, Machine: AArch64
NEEDED liblog.so, libm.so, libdl.so, libc.so
SONAME libmpg.so
LOAD p_align 0x4000 (x3)
JNI exports (14/14):
  Java_nobleworks_libmpg_MP3Decoder_initialize
  Java_nobleworks_libmpg_MP3Decoder_openFile
  Java_nobleworks_libmpg_MP3Decoder_readSamples
  Java_nobleworks_libmpg_MP3Decoder_skipSamples
  Java_nobleworks_libmpg_MP3Decoder_seekTo
  Java_nobleworks_libmpg_MP3Decoder_seekToEnd
  Java_nobleworks_libmpg_MP3Decoder_getNumChannels
  Java_nobleworks_libmpg_MP3Decoder_getRate
  Java_nobleworks_libmpg_MP3Decoder_getLength
  Java_nobleworks_libmpg_MP3Decoder_getSupportedRates
  Java_nobleworks_libmpg_MP3Decoder_getEncodings
  Java_nobleworks_libmpg_MP3Decoder_getErrorMessage
  Java_nobleworks_libmpg_MP3Decoder_delete
  Java_nobleworks_libmpg_MP3Decoder_00024Feature_isFeatureSupported
unresolved mpg123_* symbols: 0
```

### armeabi-v7a libsonic.so — actual output

`Class: ELF32`, `Machine: ARM`, `SONAME libsonic.so`,
`NEEDED liblog.so libm.so libdl.so libc.so`, 20/20 JNI symbols.
(This ABI is a reference-only build; `p_align` check applies to 64-bit ABIs
that were rebuilt with the page-size flag — v7 was not rebuilt for 16 KB.)

### x86_64 build — actual output

`libsonic.so`: `ELF64` / `X86-64`, LOAD `p_align 0x4000`, 20/20 JNI symbols;
`libmpg123.a`: all members `elf64-x86-64`, 14/14 required API;
`libmpg.so`: `ELF64` / `X86-64`, LOAD `p_align 0x4000`,
`NEEDED liblog.so libm.so libdl.so libc.so`, `SONAME libmpg.so`,
14/14 MP3Decoder JNI exports, 0 unresolved `mpg123_*`.
Verified by `build.ps1 -Abi x86_64`.

---

## 8. libmpg.so (MP3Decoder wrapper) workflow

1. The JNI C file lives here: `native\jni\mpg_jni.c`
   (Java package `nobleworks.libmpg`, class `MP3Decoder`).
2. Run:
   ```bat
   cd <repo>\native
   build.cmd -Abi arm64-v8a     :: or -Abi x86_64 for the emulator
   ```
3. `build.ps1` detects the file, builds target `mpg`, links it against
   `libmpg123.a`, and stages **`out\<abi>\libmpg.so`**. It then checks that
   `Java_nobleworks_libmpg_MP3Decoder_*` is exported, that no `mpg123_*`
   symbol is left unresolved, and that LOAD `p_align` is 0x4000.
4. `#include <mpg123.h>` works out of the box (`out\<abi>\include` is also
   the staged header dir; the target's include path points at
   `src\mpg123\src\include`).

If `jni\mpg_jni.c` is absent, the target still builds from
`jni\mpg_jni_placeholder.c` only when `-BuildMpg` is passed, and that
placeholder is written to `build\<abi>\placeholder\` — it is never installed
into `out\`. Use `-SkipMpg` to ignore the target entirely.

---

## 9. Source provenance / re-fetching

| Source | Version | URL | SHA-256 |
|---|---|---|---|
| mpg123 | 1.32.10 | `https://downloads.sourceforge.net/project/mpg123/mpg123/1.32.10/mpg123-1.32.10.tar.bz2` | `87B2C17FE0C979D3EF38EECEFF6362B35B28AC8589FBF1854B5BE75C9AB6557C` |
| sonic-ndk | master @ `4c33ea6a6c3d1778fcbdfa76e31b298fd36485b8` (2017-07-09) | `https://codeload.github.com/waywardgeek/sonic-ndk/zip/refs/heads/master` | `15BEAAA36D942FFB6E0A6261A991DBAFE41308E85BF8A3F7AF254331C557E324` |

The original archives are kept in `src\_download\`. To re-extract from scratch:

```bat
curl -L -o src\_download\mpg123-1.32.10.tar.bz2 https://downloads.sourceforge.net/project/mpg123/mpg123/1.32.10/mpg123-1.32.10.tar.bz2
curl -L -o src\_download\sonic-ndk-master.zip https://codeload.github.com/waywardgeek/sonic-ndk/zip/refs/heads/master
python -c "import tarfile; tarfile.open(r'src\_download\mpg123-1.32.10.tar.bz2','r:bz2').extractall(r'src\mpg123')"
python -c "import zipfile; zipfile.ZipFile(r'src\_download\sonic-ndk-master.zip').extractall(r'src\sonic')"
```

(then flatten `mpg123-1.32.10\` → `src\mpg123\` and `sonic-ndk-master\` →
`src\sonic\` / rename `sonic\jni` files as needed; the build expects
`src\mpg123\src\libmpg123\libmpg123.c` and `src\sonic\jni\sonic.c`).

`src\mpg123` and `src\sonic` are pristine upstream; **all** customization
lives in `native\` (`cmake\config-mpg123.h.in`, `CMakeLists.txt`).

---

## 10. Repo layout / portability notes

This `native/` directory is published inside the `mafon_re` repository and is
self-contained:

```
<repo>\native\        this CMake project (build.ps1, CMakeLists.txt, jni/)
<repo>\src\mpg123\    pristine mpg123 1.32.10 sources  (MAFON_SRC_ROOT = <repo>/src)
<repo>\src\sonic\     pristine waywardgeek/sonic-ndk sources
```

Notes:

* `build.ps1` derives `build\`, `out\` and `buildlogs\` from `$PSScriptRoot`,
  and `CMakeLists.txt` derives `MAFON_SRC_ROOT` from `native\..\src`, so the
  tree builds as-is with no absolute paths.
* The NDK is located from `-Ndk`, `$env:ANDROID_NDK_ROOT` or
  `$env:ANDROID_NDK_HOME` (r26b was used for the published binaries).
* `cmake` / `ninja` are taken from `PATH`, from the Python `Scripts` directory
  (`pip install cmake ninja`), or from `-CMake` / `-Ninja`.
* Do **not** commit `native/build/`, `native/buildlogs/` or `native/out/`
  (the directory-local `.gitignore` already excludes them); the published
  binaries live in `<repo>/dist/lib/`.
* `jni/mpg_jni.c` is the real, reverse-engineered MP3Decoder wrapper. If it is
  absent the `libmpg.so` target falls back to a placeholder that is never
  staged; `-BuildMpg` builds the placeholder explicitly.

## 11. Caveats

1. **Unstripped artifacts.** NDK clang keeps `-g` even in Release, so
   `libsonic.so` is ~112 KB (arm64) unstripped vs the original ~29 KB stripped
   32-bit lib, and `libmpg.so` is ~1.1 MB. Android Gradle Plugin strips native
   libs at packaging time (`stripDebugSymbols`); for a manual strip use
   `llvm-strip --strip-unneeded out\<abi>\libsonic.so out\<abi>\libmpg.so`.
2. **Generic C decoder only.** mpg123 is built with `OPT_GENERIC` +
   `REAL_IS_FLOAT` (no NEON64/SSE assembly), per the task. Correct but a bit
   slower than the NEON build; the public API/symbols are identical. If needed
   later, add the `dct36_neon64.S` / `synth_neon64*.S` sources and
   `-DOPT_MULTI -DOPT_NEON64` (NDK clang assembles them fine).
3. **mpg123 version difference.** 1.32.10 is much newer than whatever the
   original APK bundled (the original `libmpg.so` exports are a subset).
   The JNI wrapper only uses the stable `mpg123_*` API listed in the task —
   all present. Behavioural differences are improvements (gapless on, better
   seek/frame index, ID3v2 handling); error strings may differ from the
   original binary.
4. **Offsets.** 64-bit off_t on arm64-v8a/x86_64 with `SIZEOF_OFF_T=8` and no
   `_FILE_OFFSET_BITS`; on 32-bit targets the CMake project adds
   `-D_FILE_OFFSET_BITS=64` (not built by default here).
5. **API levels.** arm64-v8a = 21 (as required). armeabi-v7a libsonic is built
   at API 16 only for comparison; mpg123/libmpg are 64-bit ABI builds.
6. **16 KB page size.** All 64-bit `.so` files are linked with
   `-z max-page-size=16384` so they load on Android 15/16 devices with 16 KB
   pages; see §6. If you build with `-MaxPageSize 0` they fall back to 4 KB
   alignment and may fail to install/load on those devices.
7. **NEEDED libs differ from the old 32-bit libs.** Old `libsonic.so` /
   `libmpg.so` needed `libstdc++.so` (old NDK C++ runtime); these are pure C
   and need only `liblog.so, libm.so, libdl.so, libc.so`. Do not mix the new
   arm64 libs with the old 32-bit ones.
8. **Java package names must match.** `libsonic.so` exports
   `Java_org_vinuxproject_sonic_Sonic_*`; `libmpg.so` exports
   `Java_nobleworks_libmpg_MP3Decoder_*`.
9. **libmpg.so is only staged for the real wrapper.** A placeholder build is
   a pipeline check; `out\<abi>\libmpg.so` appears only after
   `native\jni\mpg_jni.c` exists (or a real `-DMAFON_JNI_SOURCE` is given).
10. **build.ps1 does not download sources**; it expects `..\src` to be
    populated. See §9 for URLs/hashes.
11. **`out\` is not wiped by default.** For a guaranteed-clean rebuild use
    `-Clean`; otherwise stale headers can remain in `out\<abi>\include\`.
12. `x86` (32-bit x86) is accepted by the parameter validation but untested;
    use `x86_64` for emulator smoke tests.
13. CMake 4.4.4 + NDK r26b: deprecation warnings from the NDK toolchain file
    are expected; builds succeed (`exit 0`).
14. `out\<abi>\libmpg.so` from the real wrapper exports the whole `mpg123_*`
    API too (default visibility, static archive linked in) — the original
    `libmpg.so` also exported these, so this matches.

---

## 12. Troubleshooting

| Symptom | Fix |
|---|---|
| `无法加载文件 ...build.ps1。未对文件进行数字签名` / "not digitally signed" | use `build.cmd ...` or `powershell -ExecutionPolicy Bypass -File .\build.ps1 ...` |
| `Cannot find 'cmake'` | `python -m pip install cmake ninja`, or pass `-CMake`/`-Ninja` |
| CMake: `mpg123 sources not found` | populate `..\src\mpg123` (see §9) |
| `sonic sources not found` | populate `..\src\sonic` (see §9) |
| `libmpg.so has unresolved mpg123_* symbols` | ensure `MAFON_BUILD_MPG123=ON` and the target links `mpg123static` (default) |
| `LOAD p_align is not 0x4000` | ensure `MAFON_MAX_PAGE_SIZE=16384` (default) and that the target uses `mafon_android_link_options()` |
| Stale/garbled build | `build.cmd -Abi <abi> -Clean` |
| Want to inspect why a step failed | `buildlogs\<abi>\build-{configure,compile,install}.log` |
