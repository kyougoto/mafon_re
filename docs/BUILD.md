# 构建指南（BUILD）

本仓库从零复现两个 arm64-v8a 原生库，并重打包成可安装的 APK。完整参数说明见
[`native/BUILD.md`](../native/BUILD.md)，这里是仓库层面的最短路径。

## 1. 前置

| 工具 | 版本 | 说明 |
| --- | --- | --- |
| Android NDK | **r26b**（26.1.10909125） | 交叉编译 `libmpg.so` / `libsonic.so` |
| CMake + Ninja | CMake ≥ 3.20（本仓库用 4.4.4），Ninja 任意 | `pip install cmake ninja` |
| JDK | 17+ | 运行 `apksigner` |
| Android SDK build-tools | 34（含 `apksigner`、`zipalign`） | 重签名 |
| Python | 3.9+ | `tools/patch_apk.py` |

NDK 查找顺序：`-Ndk <path>` → `$env:ANDROID_NDK_ROOT` → `$env:ANDROID_NDK_HOME`。
`cmake`/`ninja` 查找顺序：`PATH` → Python 的 `Scripts` 目录 → `-CMake`/`-Ninja`。

## 2. 编译原生库

```powershell
cd native
.\build.ps1 -Abi arm64-v8a          # 或 build.cmd -Abi arm64-v8a
```

产物（未 strip）：

```
native\out\arm64-v8a\libmpg.so        (mpg123 静态链接 + MP3Decoder JNI)
native\out\arm64-v8a\libsonic.so      (org.vinuxproject.sonic JNI)
native\out\arm64-v8a\libmpg123.a      (中间产物)
native\out\arm64-v8a\include\*.h
```

构建脚本自动验证：ELF 类别/机器类型、SONAME、NEEDED、JNI 导出符号、未解析的
`mpg123_*`、以及所有 `LOAD` 段是否按 16 KB 页对齐（`p_align=0x4000`）。

其他 ABI（参考/模拟器用）：`.\build.ps1 -Abi x86_64`、`-Abi armeabi-v7a`。

### 源码来源

| 组件 | 来源 | 版本 |
| --- | --- | --- |
| mpg123 | <https://www.mpg123.de/> 1.32.10 源码包 | 源码位于 `src/mpg123/` |
| sonic-ndk | <https://github.com/waywardgeek/sonic-ndk> | 源码位于 `src/sonic/` |

`native/CMakeLists.txt` 直接编译 `src/mpg123/src/libmpg123` 下的核心 C 文件为静态库
`libmpg123.a`（`-fPIC`、`-DOPT_GENERIC -DREAL_IS_FLOAT`、纯 C 解码器），再与
`native/jni/mpg_jni.c` 链接成 `libmpg.so`。

## 3. 重打包 APK

准备一份**原版** APK（`com.neosphere.mafon` 1.0/75，本仓库不附带原包），然后：

```powershell
python tools\patch_apk.py `
    --src "路径\mafon权限修改.apk" `
    --libs dist\lib\arm64-v8a `
    --out build\mafon-1.0-arm64-unsigned.apk
```

脚本只做三件事：复制原有 zip 条目（逐字节）、删除旧 v1 签名文件、追加
`lib/arm64-v8a/{libmpg.so,libsonic.so}`。原 manifest/dex/资源完全不变。

需要 Android 14/15 可安装的变体时加 `--target-sdk 25`（只改二进制
`AndroidManifest.xml` 中 `android:targetSdkVersion` 的整数值）。

## 4. 对齐与签名

```powershell
$env:JAVA_HOME   = 'C:\path\to\jdk-17'
$env:ZIPALIGN    = 'C:\path\to\build-tools\34.0.0\zipalign.exe'
$env:APKSIGNER   = 'C:\path\to\build-tools\34.0.0\apksigner.bat'
tools\sign_apk.ps1 -In build\mafon-1.0-arm64-unsigned.apk -Out dist\mafon-1.0-arm64.apk
```

脚本依次执行 `zipalign -f -p 4`、`apksigner sign`（v1+v2+v3）、`apksigner verify
--verbose --print-certs`、`zipalign -c -p 4`，任何一步失败都会报错退出。

签名密钥：`tools/mafon_re.jks`（storepass / keypass：`mafon_re`，alias：`mafon`）。
这是公开的非机密测试密钥，目的是让任何人都能用同一密钥重签、覆盖升级。若要发布
自己的版本，请替换 `-Keystore` / `-StorePass` / `-Alias`。

## 5. 目录说明

```
dist\lib\arm64-v8a\          已 strip 的发布用原生库
dist\*.apk                   已签名成品
native\                      CMake 工程（构建脚本 + JNI 源码）
src\mpg123\                  mpg123 1.32.10 原始源码
src\sonic\                   sonic-ndk 原始源码（保留 jni/ 与许可文件）
tools\                       重打包/签名脚本与公开密钥
```

## 6. 常见问题

- **`Failed to find package 'emulator'` / sdkmanager 拉不到仓库**：与本项目无关，
  直接手动下载 build-tools 与 NDK 压缩包解压即可（构建只用到其中的 clang、llvm
  binutils、apksigner、zipalign）。
- **`apksigner` 报找不到 `java`**：设置 `JAVA_HOME`（脚本会把它加入 `PATH`）。
- **`zipalign` 报 “Unable to open ... as zip archive”**：输入 APK 路径含中文/空格时
  请用引号；本仓库的 `patch_apk.py` 输出的 zip 已用标准 local header，可正常对齐。
- **`-BuildMpg` 是什么**：当 `native/jni/mpg_jni.c` 不存在时，CMake 用占位源文件
  验证目标可链接；占位产物不会进入 `out/`，因此不会被误打进 APK。
