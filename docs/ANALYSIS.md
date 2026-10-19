# 逆向分析记录（ANALYSIS）

本文记录把 Mafon（`com.neosphere.mafon`, versionName 1.0, versionCode 75）从 32 位 ARM 改造成 arm64-v8a 时的全部逆向结论，重点是没有源码的 `libmpg.so` JNI 封装。

## 1. 目标 APK 概况

| 项 | 值 |
| --- | --- |
| package | `com.neosphere.mafon` |
| versionCode / versionName | 75 / 1.0 |
| minSdkVersion / targetSdkVersion | 8 / 16 |
| 组件 | `MainActivity`（launcher）、`PlayerService`、`MediaReceiver`（MEDIA_BUTTON） |
| 权限 | `READ_EXTERNAL_STORAGE`、`WRITE_EXTERNAL_STORAGE`、`WRITE_SETTINGS`、`CHANGE_CONFIGURATION` |
| dex | 单个 `classes.dex`，649 个类 |
| 原生库 | `lib/armeabi`、`lib/armeabi-v7a`：`libmpg.so`、`libsonic.so` |
| 资源 | 卡带机皮肤 `assets/elektronika-302-2/`、`javazoom/jl/decoder/*.ser`、`res/raw/talking.bin` 等 |

Java 侧只通过两个类使用原生库：

- `nobleworks.libmpg.MP3Decoder` → `System.loadLibrary("mpg")`
- `org.vinuxproject.sonic.Sonic` → `System.loadLibrary("sonic")`

播放链路：`PlayerService` 创建 `Playerv3`；`Playerv3.DecodingThread` 用 `MP3Decoder` 解码 16-bit PCM 到直接 `ByteBuffer`（32768 字节）/ `ShortBuffer`，`Playerv3.PlayerThread` 用 `Sonic` 做变速（`putBytes`/`receiveBytes`），最后写 `AudioTrack`。`Playerv2` 在 v75 里是死代码（`PlayerService` 只实例化 `Playerv3`），且其代码有 `ByteBuffer.allocateDirect(4096).array()` 的潜在崩溃，不作为行为参考。

音乐列表来自系统 MediaStore（`ContentResolver.query(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, ...)`），也就是通过 `DATA` 列拿到文件路径后再交给 `MP3Decoder(String path)`。代码里没有 `Environment.getExternalStorageDirectory()`，也没有任何 `Build.CPU_ABI` / `os.arch` 判断——**只要 64 位库放进 `lib/arm64-v8a/`，Java 侧无需任何改动**。

## 2. libsonic.so

`libsonic.so` 的 20 个导出符号与 Java 类 `Sonic` 的 native 声明一一对应：

```
initNative(int sampleRate, int channels) -> long
closeNative(long), flushNative(long)
set/get SampleRate, NumChannels, Pitch, Speed, Rate, ChordPitch, Volume
putBytesNative(long, byte[], int) -> boolean
receiveBytesNative(long, byte[], int) -> int
availableBytesNative(long) -> int
```

这套接口与上游 [waywardgeek/sonic-ndk](https://github.com/waywardgeek/sonic-ndk) 的 `jni/sonicjni.c` 完全一致（连参数名语义都相同），因此 arm64 版本直接由上游源码重新编译即可，无需重写。

## 3. libmpg.so —— 从反汇编重建 JNI 封装

`libmpg.so` 的 JNI 源码从未公开（`Nobleworks/Android-libmpg` 仓库迁移到 GitLab 后只剩一个 README，且 GitLab 上的 `libmpg123` 只是 mpg123 内核镜像）。因此以 `armeabi-v7a/libmpg.so` 的 **ARM 反汇编**（`llvm-objdump -d --triple=armv7-none-linux-androideabi`）为准逐函数重建。

### 3.1 导出符号（与原库严格一致）

```
Java_nobleworks_libmpg_MP3Decoder_initialize()I
Java_nobleworks_libmpg_MP3Decoder_getSupportedRates()[I
Java_nobleworks_libmpg_MP3Decoder_getEncodings()[I
Java_nobleworks_libmpg_MP3Decoder_getErrorMessage(I)Ljava/lang/String;
Java_nobleworks_libmpg_MP3Decoder_00024Feature_isFeatureSupported(I)Z
Java_nobleworks_libmpg_MP3Decoder_openFile(Ljava/lang/String;)J
Java_nobleworks_libmpg_MP3Decoder_delete(J)V
Java_nobleworks_libmpg_MP3Decoder_readSamples(JLjava/nio/ShortBuffer;I)I
Java_nobleworks_libmpg_MP3Decoder_skipSamples(JI)I
Java_nobleworks_libmpg_MP3Decoder_seekTo(JI)I
Java_nobleworks_libmpg_MP3Decoder_seekToEnd(JI)I
Java_nobleworks_libmpg_MP3Decoder_getNumChannels(J)I
Java_nobleworks_libmpg_MP3Decoder_getRate(J)I
Java_nobleworks_libmpg_MP3Decoder_getLength(J)F
```

注意：dex 里还存在 `getFlags()`, `setFlags()`, `getSupportedEncodings()`, `getSupportedRates()`, `seekTo(I)` 等 Java 包装，其中 `getFlags/setFlags` 的 native 实现在原库中**并不存在**（Java 侧从未调用，属库内遗留 API）。为保持行为一致，重建版也不导出这两个符号。

另一个核对结果：原 `libmpg.so` / `libsonic.so` **都没有 `JNI_OnLoad`，也没有 `RegisterNatives`**（两个 ABI 均确认），JNI 方法完全靠符号名查找。因此重建的库只要保证导出符号名一致即可，无需任何注册逻辑。

### 3.2 Decoder 结构体（32 字节，来自 disasm 字段偏移）

```
偏移   类型                 字段
0x00   mpg123_handle *      handle
0x04   int                  channels
0x08   int                  rate
0x0c   float                length      (秒)
0x10   int/size_t           outblock    (解码缓冲区字节数)
0x14   void *               buffer
0x18   int                  samples_available   (缓冲区中剩余 16-bit 样本数)
0x1c   int                  buffer_pos          (缓冲区读位置，以 16-bit 样本计)
```

`openFile` 返回该结构体指针作为 Java 侧的 `long handle`；`0` 表示失败。

### 3.3 各函数的原始语义

- `initialize()`：`mpg123_init()`，返回值直接返回（0 成功）。Java 的 `<clinit>` 中若返回值非 0 会 `throw new Error(...)`。
- `getSupportedRates()`：`mpg123_rates(&list,&count)` → `NewIntArray(count)` → 填充 → 返回。
- `getEncodings()`：`mpg123_encodings(&list,&count)` → 把每个 mpg123 编码常量映射成 Java `Encoding` 枚举的序号，见 3.4。
- `getErrorMessage(int)`：`mpg123_plain_strerror()` → `NewStringUTF`。
- `Feature.isFeatureSupported(int)`：`mpg123_feature(feature) != 0`。
- `openFile(String)`：
  1. `mpg123_new(NULL,&err)`；handle 为空或 err≠0 返回 0。
  2. `calloc` Decoder，`GetStringUTFChars` 取路径，`mpg123_open(h,path)`，`ReleaseStringUTFChars`。
  3. `mpg123_getformat(h,&rate,&channels,&encoding)`；要求返回 OK 且 `encoding == MPG123_ENC_SIGNED_16`（否则关闭并返回 0）。
  4. `mpg123_format_none(h)` + `mpg123_format(h,rate,channels,MPG123_ENC_SIGNED_16)`：统一输出 16-bit 有符号 PCM，不重采样。
  5. `outblock = mpg123_outblock(h)`；`buffer = malloc(outblock)`。
  6. `len = mpg123_length(h)`；成功则 `length = len / rate` 秒（失败/未知置 0）。
  7. 返回 `(jlong)decoder`。
- `readSamples(long, ShortBuffer, int n)`：
  - `dst = GetDirectBufferAddress(buf)`（Java 侧传入的是 `allocateDirect()` 的 `ShortBuffer`）。
  - 循环：缓冲区空则 `mpg123_read(h, buffer, outblock, &done)`，`samples_available = done/2`；然后按 16-bit 样本 `memcpy` 到 `dst`，推进 `buffer_pos`，直到拷满 n 或 EOF/错误。
  - 返回实际拷贝的样本数；**EOF/错误时返回 0**（与原库一致）。
  - Java 包装 `readSamples(ShortBuffer)` 传入 `capacity()`，返回后把 `position` 归零。
- `skipSamples(long, int n)`：与上相同但不拷贝，返回实际跳过的样本数。
- `seekTo(long, int sample)`：`mpg123_seek(h, sample, SEEK_SET)`；`seekToEnd` 用 `SEEK_END`。
  - 重建版额外做了「清空已缓存样本」的处理（原库疏忽未清，seek 后可能先播放旧缓冲的残音）；对 Java 侧可见的返回值不变。
- `delete(long)`：`mpg123_close` + `mpg123_delete` + `free(buffer)` + `free(decoder)`。
- `getNumChannels/getRate/getLength`：直接读结构体字段。

### 3.4 编码常量 → Java 枚举序号

Java `Encoding` 枚举声明顺序（即 `Enum.ordinal()`）：

```
0 ULAW_8  1 ALAW_8  2 PCM_SIGNED_8  3 PCM_UNSIGNED_8
4 PCM_SIGNED_16  5 PCM_UNSIGNED_16  6 PCM_SIGNED_24  7 PCM_UNSIGNED_24
8 PCM_SIGNED_32  9 PCM_UNSIGNED_32  10 PCM_FLOAT_32  11 PCM_FLOAT_64
```

原库的 `convertToEncodingOrdinal()` 里硬编码了 2013 年前后 mpg123 的编码位值（`0x04→0, 0x08→1, 0x82→2, 0x01→3, 0xD0→4, 0x60→5, 0x5080→6, 0x6000→7, 0x1180→8, 0x2100→9, 0x200→10, 0x400→11`）。重建版改用符号常量 `MPG123_ENC_ULAW_8` 等做映射，避免 mpg123 新版重排编码位后映射错位；`getSupportedEncodings()` 在应用里并未被调用，此处仅为接口完整。

### 3.5 32 位 → 64 位移植注意点

原库是 armv7（ILP32），重建版是 arm64（LP64），以下类型宽度变化必须显式处理：

- `jlong` 句柄在 arm64 上是 64 位指针 → 用 `(intptr_t)` 转换，不能截断成 `int`。
- `off_t` / `long` 在 arm64 上是 64 位：`mpg123_length()` / `mpg123_seek()` 的返回值与参数用 `off_t`（64 位），Java 侧的 `int` 参数显式转换。
- `mpg123_outblock()` 返回 `size_t`（64 位），按 `size_t` 保存。
- 原库 `MP3Decoder.getLength()` 用 32 位整数除法（秒数被截断成整数）；重建版用浮点除法（更正确，且该 API 未被应用调用）。
- 原库链接了 `libstdc++`；重建版为纯 C，只依赖 `liblog/libm/libdl/libc`。

### 3.6 与原库的行为差异（有意为之）

| 差异 | 原因 |
| --- | --- |
| `seekTo/seekToEnd` 前清空已缓存的样本 | 修正原库 seek 后先播放残音的缺陷；返回值不变 |
| `getLength()` 返回浮点秒而非截断整数 | 更符合 Java 层 `float getLength()` 的语义；应用未使用 |
| 编码序号映射改用符号常量 | 兼容 mpg123 新版编码位定义 |
| 所有 `malloc`/JNI 调用增加了失败检查 | 防御性改进，正常路径行为不变 |

## 4. 重打包与签名

- 只在 zip 内新增 `lib/arm64-v8a/{libmpg.so,libsonic.so}`；其余条目逐字节复制（保留原压缩方式、时间戳、属性）。
- 删除旧签名文件（`META-INF/MANIFEST.MF`、`ANDROID.SF`、`ANDROID.RSA`），由 `apksigner` 重新生成 v1 + v2 + v3 签名。
  原始 APK 实际是 v1 + v2 签名（AOSP testkey，`CN=Android`），重签后升级为 v1 + v2 + v3。
- `zipalign -p 4` 保证 4 字节对齐（`.so` 在 APK 内为压缩存储，安装时解压，故不依赖 APK 内的页对齐；ELF 自身按 16 KB `p_align` 构建以适配 16 KB 页内核）。
- 公开密钥：`tools/mafon_re.jks`（alias `mafon`，storepass/keypass `mafon_re`）。使用公开密钥是为了让任何人能重现完全相同的签名配置并发布可覆盖升级的版本；它不保护任何机密。

## 5. targetSdk 与安装限制

- Android 14 起，`targetSdkVersion < 23` 的应用会被系统拒绝安装（`INSTALL_FAILED_DEPRECATED_SDK_VERSION`）；Android 15 起阈值提高到 24。
- 因此 `dist/mafon-1.0-arm64-sdk25.apk` 在二进制清单里把 `targetSdkVersion` 从 16 改为 25（`patch_apk.py --target-sdk 25` 只改 `uses-sdk` 属性的 32 位数据，不动任何 chunk 偏移）。
  选择 25 而不是 28/29 的原因：API 26 才开始限制后台 Service，而本应用用 `PlayerService` 做后台播放，targetSdk 25 可以保留原来的后台行为；同时 25 ≥ 24 满足 Android 15 的安装门槛。
- targetSdk ≥ 23 后存储权限改为运行时权限，应用没有 `requestPermissions` 调用（老代码），所以需要用户到系统设置里手动授予；授予后仍走 legacy 存储模型（targetSdk < 29，Android 10+ 不会强制 scoped storage），MediaStore 的 `DATA` 路径可直接 `fopen`，与原逻辑一致。
