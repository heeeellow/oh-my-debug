# scrcpy Windows x64 4.1

来源：[官方 v4.1 发布](https://github.com/Genymobile/scrcpy/releases/tag/v4.1)。
归档：`scrcpy-win64-v4.1.zip`，SHA-256：
`5b12172b3264b2889f4583ee64752ce832e29bc8b1089dca81093459697165db`。
与 [该版本 Windows 文档](https://github.com/Genymobile/scrcpy/blob/v4.1/doc/windows.md) 一致。

客户端、server、DLL、图标及 LICENSE.txt 均保持归档原字节。删除自带 ADB、两个 ADB DLL 和启动脚本，
由 TestTools 为子进程指定统一 ADB。`SHA256SUMS.txt` 覆盖保留文件、元数据和补充许可证，不包含清单自身。

依赖及对应源码：

- scrcpy 4.1（Apache-2.0）：[源码](https://github.com/Genymobile/scrcpy/tree/v4.1)，许可为 LICENSE.txt。
- FFmpeg 8.1.2（LGPL-2.1-or-later）：[源码归档](https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz)，许可为 licenses/FFmpeg-LGPL-2.1.txt。
- SDL 3.4.12（zlib）：[源码](https://github.com/libsdl-org/SDL/tree/release-3.4.12)，许可为 licenses/SDL-zlib.txt。
- libusb 1.0.30（LGPL-2.1-or-later）：[源码](https://github.com/libusb/libusb/tree/v1.0.30)，许可为 licenses/libusb-LGPL-2.1.txt。
- dav1d 1.5.3（BSD-2-Clause，静态链接于 FFmpeg）：[源码](https://code.videolan.org/videolan/dav1d/-/tree/1.5.3)，许可为 licenses/dav1d-BSD.txt。
- zlib（zlib，FFmpeg 静态依赖）：[源码](https://github.com/madler/zlib)，许可为 licenses/zlib.txt；上游交叉构建脚本未单独固定其版本。

依赖配置以 [v4.1 构建脚本](https://github.com/Genymobile/scrcpy/tree/v4.1/app/deps) 为准。
升级时重新核验归档与文件摘要，保持客户端/server 匹配，并复核依赖与许可。
