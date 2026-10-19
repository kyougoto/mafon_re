# 验证记录（VERIFICATION）

日期：2026-10-19。构建机：Windows 10 22H2 (19045)，Intel i5-10400，无 ARM64 设备/模拟器加速条件。
工具链：NDK r26b (26.1.10909125)、mpg123 1.32.10、waywardgeek/sonic-ndk、JDK 17 (Corretto 17.0.20.12.1)、Android build-tools 34.0.0。

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
c4699785a37c99213332b84db2af9efd3fe68bac44a1f7976909c12c4632901b  libmpg.so    (312224 B)
8bd5b6aa965544bbf171229d1c714130f62c71de3f6c000f0b255cd971fba7c1  libsonic.so  ( 24368 B)
```

`llvm-readelf -h`：

```
libmpg.so    Class: ELF64   Data: 2's complement, little endian   Type: DYN   Machine: AArch64
libsonic.so  Class: ELF64   Data: 2's complement, little endian   Type: DYN   Machine: AArch64
```

`llvm-readelf -l`（16 KB 页对齐，Android 15/16 的 16 KB-page 设备需要）：

```
libmpg.so    LOAD 0x000000 ... 0x04a660 R E 0x4000
             LOAD 0x04a660 ... 0x0012f8 RW  0x4000
             LOAD 0x04b958 ... 0x000278 RW  0x4000
libsonic.so  LOAD 0x000000 ... 0x005450 R E 0x4000
             LOAD 0x005450 ... 0x0002d8 RW  0x4000
```

`llvm-readelf -d`：

```
libmpg.so    SONAME libmpg.so;    NEEDED liblog.so, libm.so, libdl.so, libc.so
libsonic.so  SONAME libsonic.so;  NEEDED liblog.so, libm.so, libdl.so, libc.so
```

> 原 32 位库还依赖 `libstdc++`；新库是纯 C，不再需要。

JNI 导出（`llvm-nm -D | grep Java_`）：

```
libmpg.so    : 14 个 Java_nobleworks_libmpg_MP3Decoder_*   （与原 32 位库符号集合一致）
libsonic.so  : 20 个 Java_org_vinuxproject_sonic_Sonic_*    （与原 32 位库符号集合一致）
```

## 3. APK

| 文件 | SHA-256 |
| --- | --- |
| `dist/mafon-1.0-arm64.apk` | `fea359fbab47df5aff9f69872cd1fa4a3e6cc6fc3aeb69544bc2a827dff512ad` |
| `dist/mafon-1.0-arm64-sdk25.apk` | `256dae50cc7f4189f257be2f19eda1c542609a15f72e8de3297c95689fb48fbc` |

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
`AndroidManifest.xml` 中 `targetSdkVersion` 的 4 字节整数值。

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
（base 变体）      package: com.neosphere.mafon versionCode=75 versionName=1.0
                   sdkVersion:'8'   targetSdkVersion:'16'
（sdk25 变体）      sdkVersion:'8'   targetSdkVersion:'25'
launchable-activity: com.neosphere.mafon.MainActivity   label='Mafon'
```

## 4. 未能完成的验证：真机/模拟器端到端试听

构建机没有 ARM64 Android 设备；Windows 10 主机未启用 Hyper-V/WHPX，Android 模拟器只能以
TCG 纯软件模拟运行，未能在合理时间内完成启动验证。因此以下内容**未做运行时验证**：

- 应用在 arm64 设备上 `System.loadLibrary("mpg"/"sonic")` 是否成功；
- 实际解码/变速播放是否正常；
- `sdk25` 变体手动授权存储后 MediaStore 列表是否正常。

已做的是源码级/二进制级验证（JNI 封装按反汇编重建、符号与 ABI 正确、签名与对齐有效）。
如需真机复验，请按下面步骤操作（Windows PowerShell）：

```powershell
# 1) 连接手机（开启 USB 调试），确认设备
adb devices -l

# 2) 卸载旧版（签名不同，不能覆盖安装）
adb uninstall com.neosphere.mafon

# 3) 安装
adb install -r dist\mafon-1.0-arm64.apk
#   Android 14/15 报 INSTALL_FAILED_DEPRECATED_SDK_VERSION 时改用：
adb install -r dist\mafon-1.0-arm64-sdk25.apk
#   sdk25 变体还需要授权存储：
adb shell pm grant com.neosphere.mafon android.permission.READ_EXTERNAL_STORAGE
adb shell pm grant com.neosphere.mafon android.permission.WRITE_EXTERNAL_STORAGE

# 4) 确认加载的是 64 位库
adb shell run-as com.neosphere.mafon ls lib   # 或查看 /data/app/.../lib/arm64
adb logcat -c; adb shell am start -n com.neosphere.mafon/.MainActivity
adb logcat | Select-String 'mpg|sonic|mafon|DEBUG|AndroidRuntime'

# 5) 放一个 mp3 并触发媒体扫描后试听
adb push test.mp3 /sdcard/Music/
adb shell am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file:///sdcard/Music/test.mp3
```

判定标准：`logcat` 无 `UnsatisfiedLinkError` / `dlopen failed` / `AndroidRuntime` 崩溃；
拖动音调/速度滑块声音正常；`dumpsys package com.neosphere.mafon | findstr primaryCpuAbi`
显示 `arm64-v8a`。

## 5. 复现性

`native/build.ps1 -Abi arm64-v8a` 从干净目录重复构建两次，产物 SHA-256 稳定
（`native/out/arm64-v8a/libmpg.so` 1,128,456 B / `libsonic.so` 112,136 B，未 strip）。
`dist/lib/arm64-v8a/` 中的文件就是本仓库 `native/build.ps1` 构建产物经 `llvm-strip
--strip-unneeded` 后的结果，即上文第 2 节的哈希。原始 APK 的重打包结果也可以完全复现（zip 条目顺序与内容确定）。
