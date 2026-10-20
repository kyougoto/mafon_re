# 验证记录（VERIFICATION）

日期：2026-10-19 / 2026-10-20。构建机：Windows 10 22H2 (19045)，Intel i5-10400。
工具链：NDK r26b (26.1.10909125)、mpg123 1.32.10、waywardgeek/sonic-ndk、JDK 17 (Corretto 17.0.20.12.1)、Android build-tools 34.0.0。
真机：红魔 10 Pro+（nubia NX789J，Android 16 / API 36，CPU ABI 仅 `arm64-v8a`，无 32 位兼容层）。

## 1. 输入

| 文件 | SHA-256 |
| --- | --- |
| 原始 APK（`mafon权限修改.apk`，`com.neosphere.mafon` 1.0/75） | `af4c66c95e4b49ce28247050c9b63426a16c9736bafbcc2abbe23d49e1fd9efd` |

原始 APK 的签名：`apksigner verify --print-certs` 显示 v1=true、v2=true、v3=false，证书为
AOSP testkey（`CN=Android, O=Android, C=US`，SHA-256 `a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc`），
因此重打包后无法用原签名覆盖安装，必须先卸载原版。

```
package: com.neosphere.mafon   versionCode 75   versionName 1.0
minSdkVersion 8   targetSdkVersion 16
lib/: armeabi, armeabi-v7a  (libmpg.so, libsonic.so)
```

## 2. 原生库（`dist/lib/arm64-v8a/`，已 strip）

```
e148b22454bdaeff6c7222ac3cfde953f1bddc6f6832b589191409c3d8212d82  libmpg.so    (325096 B)
8bd5b6aa965544bbf171229d1c714130f62c71de3f6c000f0b255cd971fba7c1  libsonic.so  ( 24368 B)
```

`llvm-readelf -h`：

```
libmpg.so    Class: ELF64   Data: 2's complement, little endian   Type: DYN   Machine: AArch64
libsonic.so  Class: ELF64   Data: 2's complement, little endian   Type: DYN   Machine: AArch64
```

`llvm-readelf -l`（16 KB 页对齐，Android 15/16 的 16 KB-page 设备需要）：

```
libmpg.so    LOAD 0x000000 ... 0x04ce90 R E 0x4000
             LOAD 0x04ce90 ... 0x001458 RW  0x4000
             LOAD 0x04e2e8 ... 0x000278 RW  0x4000
libsonic.so  LOAD 0x000000 ... 0x005450 R E 0x4000
             LOAD 0x005450 ... 0x0002d8 RW  0x4000
```

`llvm-readelf -d`：

```
libmpg.so    SONAME libmpg.so;
             NEEDED liblog.so, libm.so, libmediandk.so, libdl.so, libc.so
libsonic.so  SONAME libsonic.so;
             NEEDED liblog.so, libm.so, libdl.so, libc.so
```

> 原 32 位库依赖 `libstdc++`；新库是纯 C。`libmpg.so` 新增 `libmediandk.so` 依赖，用于
> m4a/AAC/flac/ogg 等格式的 MediaCodec 回退解码（`libmediandk` 自 Android 5.0/API 21 起提供，
> 任何 arm64 设备都满足）。

JNI 导出（`llvm-nm -D | grep Java_`）：

```
libmpg.so    : 14 个 Java_nobleworks_libmpg_MP3Decoder_*   （与原 32 位库符号集合一致）
libsonic.so  : 20 个 Java_org_vinuxproject_sonic_Sonic_*    （与原 32 位库符号集合一致）
```

## 3. APK

| 文件 | SHA-256 |
| --- | --- |
| `dist/mafon-1.0-arm64.apk` | `24e95befe4015be2203b7b0c272d707207ace3000ec459a0198c0255e1330a53` |
| `dist/mafon-1.0-arm64-sdk25.apk` | `a6faee00eb96df7c4ca6c79877c27c8e796b281d262f78de3ba597adfe409a1b` |

两个 APK 均为 5,738,983 字节，versionCode 80、versionName 1.0。base 变体 targetSdk 16，sdk25 变体 targetSdk 25。

### 3.1 与原始 APK 的差异（逐条目对比 CRC 与内容 SHA-256）

```
original entries: 111   new entries: 113
added:   META-INF/MAFON.RSA, META-INF/MAFON.SF,
         lib/arm64-v8a/libmpg.so, lib/arm64-v8a/libsonic.so
removed: META-INF/ANDROID.RSA, META-INF/ANDROID.SF        （旧 v1 签名，重新签名时被替换）
changed: META-INF/MANIFEST.MF                             （签名清单，apksigner 重新生成）
```

其余 110 个条目（`AndroidManifest.xml`、`classes.dex`、`resources.arsc`、`res/`、`assets/`、
`lib/armeabi*/` 等）**内容完全一致**（SHA-256 逐条相同）。对 `sdk25` 变体，唯一额外差异是
`AndroidManifest.xml` 中 `targetSdkVersion` 的 4 字节整数值；两变体的 `versionCode` 均由 75 改为 80
（同样是清单里的 4 字节整数），以便在已安装旧包上直接覆盖升级。

### 3.2 签名

`apksigner verify --print-certs dist/mafon-1.0-arm64.apk`：

```
Verified using v1 scheme (JAR signing): true
Verified using v2 scheme (APK Signature Scheme v2): true
Verified using v3 scheme (APK Signature Scheme v3): true
Number of signers: 1
Signer #1 certificate DN: CN=Mafon ARM64, O=mafon_re, C=CN
Signer #1 certificate SHA-256 digest: ebb6de8bd2b98f088c431770c48e50b563dc0e25a91f8897cd9a17c43f13a665
```

`zipalign -c -p 4`：exit code 0（对齐检查通过）。

### 3.3 清单解析

`aapt2 dump badging`：

```
（base 变体）      package: com.neosphere.mafon versionCode='80' versionName='1.0'
                   sdkVersion:'8'   targetSdkVersion:'16'
（sdk25 变体）      sdkVersion:'8'   targetSdkVersion:'25'
launchable-activity: com.neosphere.mafon.MainActivity   label='Mafon'
（两个 APK 的 ABIs）arm64-v8a, armeabi, armeabi-v7a
```

## 4. 真机验证（红魔 10 Pro+ / Android 16 / 仅 arm64-v8a）

设备：`nubia NX789J`，Android 16（SDK 36），`ro.product.cpu.abilist = arm64-v8a`。
安装 sdk25 变体（Android 14+ 需要 targetSdk ≥ 23，Android 15+ 需要 ≥ 24）：

```
adb install -r dist/mafon-1.0-arm64-sdk25.apk        # Success
adb shell dumpsys package com.neosphere.mafon
    primaryCpuAbi=arm64-v8a
    versionCode=80 minSdk=8 targetSdk=25
```

`libmpg.so` 内置的 `media_decoder.c` 会写诊断日志到
`Android/data/com.neosphere.mafon/files/mafon_media.log`（该 ROM 默认关闭 adb logcat）。播放一个真实录音
`.m4a`（8 kHz 单声道 AAC-LC；为保护隐私，下面日志中的文件名已隐去）后的日志：

```
[mafon] open request: /storage/emulated/0/CallRecord/<redacted>.m4a
[mafon] data source -> 0
[mafon] track: mime=audio/mp4a-latm rate=8000 channels=1 duration_us=26728000
               format=mime: string(audio/mp4a-latm), ..., channel-count: int32(1),
                      sample-rate: int32(8000), csd-0: data, ... } 
[mafon] createDecoderByType(audio/mp4a-latm) -> 0x...
[mafon] configure -> 0
[mafon] start -> 0
[mafon] opened ... mime=audio/mp4a-latm rate_in=8000 channels_in=1 duration=26.728s
[mafon] output format: rate=8000 channels=1 pcm-encoding=2
（随后输入缓冲持续被消费，pts 从 0 递增到整轨结束，最后 queue EOS -> 0）
```

结论：

- arm64 库在 64 位真机上正常加载（`primaryCpuAbi=arm64-v8a`），应用无 native 崩溃、无 `UnsatisfiedLinkError`；
- m4a 文件可被 MediaStore 扫描到，并且**可以正常播放**（机主实测确认）；
- `targetSdk 25` 变体在 Android 16 上安装、运行正常；
- 播放过程中出现的短暂 audio-focus 变化来自测试期间的人为操作（机主同时在使用手机），不是播放故障。

### 4.1 Android 15+ NDK MediaCodec 的注意事项（重要）

Android 15/16 上，NDK `AMediaCodec` 在**没有 JavaVM 的纯 native 进程**里不会产出输出缓冲
（`dequeueOutputBuffer` 永远 `TRY_AGAIN`）。本项目的 `adb shell` 测试二进制（`native/jni/media_test.c`）
复现了该现象；同一个 `media_decoder.c` 在 **Mafon 应用进程（有 JVM）** 中解码正常。这与
[FFmpeg #11363](https://trac.ffmpeg.org/ticket/11363)、
[Mozilla bug 1987001](https://bugzilla.mozilla.org/show_bug.cgi?id=1987001) 的描述一致。

因此：本项目选择把解码器放在应用进程内（`libmpg.so` 由应用加载），而不是做成独立进程/命令行工具；
`media_test.c` 仅用于 Android 14 及以下或包成 app 时的诊断。

## 5. 复现性

`native/build.ps1 -Abi arm64-v8a` 从干净目录重复构建，未 strip 产物：

```
libmpg.so    1,174,720 B
libsonic.so    112,136 B
libmpg123.a  1,858,974 B
```

`dist/lib/arm64-v8a/` 中的文件就是上述构建产物经 `llvm-strip --strip-unneeded` 后的结果
（即第 2 节的哈希）。原始 APK 重打包 + `zipalign` + `apksigner` 的结果可完全复现（zip 条目顺序与内容确定）。

## 6. 复验步骤（其他设备）

```powershell
adb devices -l
adb install -r dist\mafon-1.0-arm64-sdk25.apk   # Android 14+；旧系统可用 base 变体
adb shell pm grant com.neosphere.mafon android.permission.READ_EXTERNAL_STORAGE  # sdk25 需要
adb shell am start -n com.neosphere.mafon/.MainActivity

# 若 ROM 允许 logcat：
adb logcat -s mafon-mpg:V
# 否则看应用内日志：
adb shell cat /storage/emulated/0/Android/data/com.neosphere.mafon/files/mafon_media.log

# 确认 64 位：
adb shell dumpsys package com.neosphere.mafon | findstr primaryCpuAbi
```

判定标准：`primaryCpuAbi=arm64-v8a`；播放 m4a/mp3 无崩溃；日志里出现 `opened ... mime=audio/...`。
