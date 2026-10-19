# mafon_re — 让 «Мафон» / Mafon 在 arm64 设备上运行

把只带 32 位 ARM 原生库的 Android 应用 **Mafon**（`com.neosphere.mafon`，versionName `1.0`，versionCode `75`，minSdk 8，targetSdk 16）改造为 **arm64-v8a (64 位)** 可用的 APK。

> 应用本体：俄罗斯开发者 neosphere 的卡带机模拟播放器（Электроника-302-2 皮肤 + Sonic 变速 + mpg123 解码）。
> 本仓库不包含应用源码，只包含：重新编译的 64 位原生库、逆向得到的 JNI 封装源码、可复现的构建脚本、重打包说明以及可安装的成品 APK。

---

## 问题

原 APK 的 `lib/` 只有两个 ABI：

```
lib/armeabi/libmpg.so        lib/armeabi/libsonic.so
lib/armeabi-v7a/libmpg.so    lib/armeabi-v7a/libsonic.so
```

| 原生库 | 作用 | 对应的 Java 类 |
| --- | --- | --- |
| `libmpg.so` | mpg123 解码器的 JNI 封装 | `nobleworks.libmpg.MP3Decoder` |
| `libsonic.so` | Sonic 变速/变调库的 JNI 封装 | `org.vinuxproject.sonic.Sonic` |

在没有 32 位兼容层的 64 位设备（Pixel 7 及之后、骁龙 8 Gen 3 之后的多数机型等）上，安装会失败（`INSTALL_FAILED_NO_MATCHING_ABIS`）或运行即崩溃（`UnsatisfiedLinkError`）。

## 方案

1. **用 Android NDK r26b 交叉编译出 arm64-v8a 的原生库**：
   - `libsonic.so`：直接使用上游 [waywardgeek/sonic-ndk](https://github.com/waywardgeek/sonic-ndk) 源码。原 APK 的 JNI 封装与该仓库完全一致（20 个 native 方法签名、`Sonic(int,int)` 构造、`initNative/putBytesNative/...` 逐个核对）。
   - `libmpg.so`：原 JNI 源码未公开（[Nobleworks/Android-libmpg](https://github.com/Nobleworks/Android-libmpg) 已迁移且只剩 README），因此**按 armeabi-v7a 版 .so 的反汇编逐函数重建** JNI 封装：`Decoder` 结构体布局、`openFile` 的格式协商、`readSamples`/`skipSamples` 的缓冲 refill 逻辑、`seekTo`/`seekToEnd` 语义等，见 [native/jni/mpg_jni.c](native/jni/mpg_jni.c) 与 [docs/ANALYSIS.md](docs/ANALYSIS.md)。mpg123 采用 **1.32.10** 源码静态链接进 `libmpg.so`（只用到稳定的公共 API）。
2. **最小化改动 APK**：只在 zip 中新增 `lib/arm64-v8a/`，原有 `AndroidManifest.xml`、`classes.dex`、`resources.arsc`、`res/`、`assets/` 与 32 位库全部逐字节保留（32 位设备行为不变）。
3. **重新签名**：`zipalign` + `apksigner`（v1 + v2 + v3），密钥随仓库公开（非机密测试密钥），保证后续用同一密钥重签可覆盖升级。

## 成品 APK

| 文件 | 说明 | 适用系统 |
| --- | --- | --- |
| [`dist/mafon-1.0-arm64.apk`](dist/mafon-1.0-arm64.apk) | 只新增 arm64 库，**不改任何清单/行为** | Android 13 及以下；任何不再限制 targetSdk 的系统 |
| [`dist/mafon-1.0-arm64-sdk25.apk`](dist/mafon-1.0-arm64-sdk25.apk) | 同上，另外把 `android:targetSdkVersion` 由 16 提升到 25 | Android 14/15（Android 14 起禁止安装 targetSdk < 23 的应用，Android 15 起 < 24） |

也可以直接从 [GitHub Releases](https://github.com/kyougoto/mafon_re/releases/tag/v1.0-arm64) 下载这两个 APK。

安装：

```powershell
adb install -r dist/mafon-1.0-arm64.apk
# 若报 INSTALL_FAILED_DEPRECATED_SDK_VERSION：
adb install -r dist/mafon-1.0-arm64-sdk25.apk
```

使用注意：

- `sdk25` 版本因为 targetSdk ≥ 23，**存储权限不再安装即授予**。安装后到
  `设置 → 应用 → Mafon → 权限 → 存储/音乐和音频` 手动允许，否则歌曲列表为空。
  （也可用 adb：`adb shell pm grant com.neosphere.mafon android.permission.READ_EXTERNAL_STORAGE`）
- 原版 `targetSdk 16` 在 Android 14+ 上会被系统直接拒绝安装，这不是 arm64 改造造成的，是 Google 的安装限制；因此提供 `sdk25` 变体。
- 应用通过系统 MediaStore 读取音乐。刚拷贝进手机的 mp3 若尚未被媒体扫描收录，列表里不会出现（可用系统播放器/文件管理器触发扫描，或重启手机）。

## 仓库结构

```
mafon_re/
├── dist/                         成品（已签名 APK + arm64 原生库）
│   ├── mafon-1.0-arm64.apk
│   ├── mafon-1.0-arm64-sdk25.apk
│   └── lib/arm64-v8a/{libmpg.so,libsonic.so}
├── native/                       NDK/CMake 工程（构建 libmpg.so / libsonic.so）
│   ├── CMakeLists.txt
│   ├── build.ps1 / build.cmd
│   └── jni/mpg_jni.c             ← 逆向重建的 MP3Decoder JNI 封装（本项目的核心代码）
├── src/
│   ├── mpg123/                   mpg123 1.32.10 原始源码（LGPL-2.1）
│   └── sonic/                    waywardgeek/sonic-ndk 原始源码（LGPL-2.1）
├── tools/
│   ├── patch_apk.py              重打包：加 arm64 库 / 可选改 targetSdk
│   ├── sign_apk.ps1              zipalign + apksigner + 校验
│   └── mafon_re.jks              公开测试签名密钥（storepass/keypass: mafon_re，alias: mafon）
└── docs/
    ├── ANALYSIS.md               逆向分析：JNI 接口、结构体、函数语义
    ├── BUILD.md                  从零复现构建
    └── VERIFICATION.md           验证记录
```

## 从零复现

前置：JDK 17、Android SDK build-tools 34（`apksigner`/`zipalign`）、Android NDK r26b、Python 3、CMake + Ninja（`pip install cmake ninja`）。

```powershell
# 1) 构建原生库（arm64-v8a）
cd native
.\build.ps1                      # 输出 native\out\arm64-v8a\{libmpg.so,libsonic.so}

# 2) 用你自己的原始 APK 重打包
python ..\tools\patch_apk.py --src <原版.apk> --libs ..\native\out\arm64-v8a `
    --out ..\build\mafon-arm64-unsigned.apk

# 3) 对齐 + 签名 + 校验
..\tools\sign_apk.ps1 -In ..\build\mafon-arm64-unsigned.apk -Out ..\dist\mafon-arm64.apk

# Android 14/15 用变体：加 --target-sdk 25
```

详细步骤见 [docs/BUILD.md](docs/BUILD.md)。

## 验证

- `llvm-readelf`：两个 `.so` 均为 `ELF64 / AArch64 / DYN`，`SONAME` 正确，仅依赖 `liblog/libm/libdl/libc`（不再需要 `libstdc++`），`LOAD` 段按 16 KB 页对齐。
- `llvm-nm -D`：`libmpg.so` 导出 14 个 `Java_nobleworks_libmpg_MP3Decoder_*`，`libsonic.so` 导出 20 个 `Java_org_vinuxproject_sonic_Sonic_*`，与原库符号集合一致。
- `apksigner verify --print-certs`：v1/v2/v3 签名均通过；`zipalign -c -p 4` 通过；`aapt2 dump badging` 可正确解析包名/版本/targetSdk。
- 未在物理 arm64 设备上做端到端试听（构建机没有此类设备），如有需要可按 [docs/VERIFICATION.md](docs/VERIFICATION.md) 的步骤在手机上复验。

## 第三方组件 / 许可

| 组件 | 版本 | 许可 |
| --- | --- | --- |
| mpg123（静态链接进 `libmpg.so`） | 1.32.10 | LGPL-2.1 |
| sonic-ndk（`libsonic.so`） | master | LGPL-2.1 |
| 本仓库新增代码（`native/jni/mpg_jni.c`、`tools/*`、文档） | — | MIT |

因为 `libmpg.so` 静态链接了 LGPL 的 mpg123，本仓库同时提供 mpg123 完整源码与可复现构建脚本，以满足 LGPL 关于重新链接的要求。

## 免责声明

本项目仅用于让用户在自己已合法拥有的应用上恢复 64 位兼容性，不包含应用原始资源、dex 或源码。重签名后的 APK 与原版签名不同，**不能直接覆盖安装原版**（需先卸载原版；卸载会清除应用数据）。
