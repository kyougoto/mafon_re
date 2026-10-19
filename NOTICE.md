# 第三方组件与许可（NOTICE）

本仓库新增的代码（`native/jni/mpg_jni.c`、`native/CMakeLists.txt`、`native/build.ps1`、
`tools/*`、文档）以 MIT 许可发布，见 [LICENSE](LICENSE)。

仓库中内嵌/链接的第三方组件：

## mpg123 1.32.10

- 目录：`src/mpg123/`
- 许可：**LGPL-2.1**（见 `src/mpg123/COPYING`）
- 用途：`libmpg.so` 静态链接了 `libmpg123.a`。
- LGPL 合规说明：本仓库随源码提供完整的 mpg123 源码、CMake 工程与构建脚本，
  任何人可以修改 mpg123 并重新构建 `libmpg.so`（`native/build.ps1`），满足 LGPL
  关于静态链接时必须允许重新链接的要求。发布 `libmpg.so` 时请一并提供本仓库源码。

## sonic-ndk (waywardgeek/sonic-ndk)

- 目录：`src/sonic/`
- 许可：**LGPL-2.1**（见 `src/sonic/COPYING`）
- 用途：编译为 `libsonic.so`（`org.vinuxproject.sonic` JNI 封装 + Sonic 变速库）。
- 上游：<https://github.com/waywardgeek/sonic-ndk>

## 应用本体

Mafon（`com.neosphere.mafon`）的原始 APK、dex、资源、图片、音频素材版权归原作者所有。
本仓库**不包含**原始 APK 或其反编译产物；`tools/patch_apk.py` 需要用户自备原始 APK，
仅在其基础上做最小改动（新增 arm64-v8a 原生库并重新签名）。重签名后的 APK 仅供
在用户自己合法拥有的应用上恢复 64 位兼容性使用。
