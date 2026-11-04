# 第三方依赖

[返回项目首页](../README.md)

当前依赖以 vendor 源码方式纳入主仓库，保证首次克隆即可构建。升级依赖时必须记录新的上游提交、
重新构建并运行自测，不直接覆盖本地适配。

| 依赖 | 上游 | 基线提交 | 本地适配 |
| --- | --- | --- | --- |
| CSerialPort | <https://github.com/itas109/CSerialPort.git> | `cb8c8b4bb358c80a01f0605367a30b07643863cb` | 增加可选的 `setControlLineChecked`，Windows 返回控制线操作结果并拒绝握手冲突；旧 void 接口保留 |
| standalone Asio | <https://github.com/chriskohlhoff/asio.git> | `8806a6803cde7054c3049d3666d3ec36786568c5` | 无 |
| tiny-process-library | <https://github.com/eidheim/tiny-process-library.git> | `4c4f1c68e90423e0b43091deeebcdb6c0fc02fb2` | `process_win.cpp` 为 Qt GUI 后台命令补齐有效 stdin，并使用 `CREATE_NO_WINDOW` |
| Android SDK Platform-Tools（精简 Windows ADB） | [官方下载](https://dl.google.com/android/repository/platform-tools_r36.0.0-win.zip) · [上游说明](https://developer.android.com/tools/releases/platform-tools) | `36.0.0-13206524`，ADB 客户端版本 `1.0.41` | 无修改；仅保留 `adb.exe`、两个配套 DLL、`NOTICE.txt`、`source.properties` |
| scrcpy（Windows x64） | [官方发布](https://github.com/Genymobile/scrcpy/releases/tag/v4.1) | `4.1` | 无修改；保留客户端、server、DLL 和图标，移除重复 ADB 与启动脚本，补齐依赖许可 |
| libvterm（纯 C VT 核心） | [上游与固定发行包](https://www.leonerd.org.uk/code/libvterm/) | `0.3.3`（MIT） | 原始 `src/` 与 `include/` 无修改，MSVC C17 静态编入桌面 |
| Phosphor Icons（内置 SVG） | [上游](https://github.com/phosphor-icons/core) · [固定发行包](https://registry.npmjs.org/@phosphor-icons/core/-/core-2.1.1.tgz) | `@phosphor-icons/core@2.1.1`（MIT，按发行包固定） | 只保留使用的 Bold/Fill/Duotone 图标，图形路径不变；运行时统一填色，样式表控件派生仅替换填色 |

ADB 固定归档 SHA-256：`12c2841f354e92a0eb2fd7bf6f0f9bf8538abce7bd6b060ac8349d6f6a61107c`。
提取文件的摘要保存在 [`third_party/platform-tools/SHA256SUMS.txt`](../third_party/platform-tools/SHA256SUMS.txt)，
完整上游声明随包保存在 [`NOTICE.txt`](../third_party/platform-tools/NOTICE.txt)。不混入本机 SDK 中其他版本的 DLL。
该目录是受控的预编译依赖，构建时直接复制，不自动升级；升级后更新版本、摘要并核对 ADB 枚举与终端启动。

每个依赖的许可证文件保留在其对应目录中。
Phosphor 的发行包 SHA-512 完整性、提取范围见 [版本记录](../third_party/phosphor-icons/VERSION.md)，
文件摘要见 [SHA256SUMS](../third_party/phosphor-icons/SHA256SUMS.txt)，许可见 [LICENSE](../third_party/phosphor-icons/LICENSE)。
SVG 通过 Qt Resource 编译进主程序，构建不需要 npm 或联网；许可证复制到输出目录的 `licenses/Phosphor-Icons-LICENSE.txt`。
桌面增加 Qt SVG 模块用于按 DPR 渲染；Qt 依赖仍限于 UI 层。
scrcpy 归档摘要、保留文件、依赖源码和补充许可见 [随包版本说明](../third_party/scrcpy/VERSION.md)，
文件摘要见 [SHA256SUMS](../third_party/scrcpy/SHA256SUMS.txt)。构建仅复制固定工具，不下载或自动升级。

原嵌套仓库的 Git 元数据仅在当前开发机上备份到主仓库
`.git/vendor-repo-metadata/`，不属于提交内容。

libvterm 的归档来源、SHA-256 与提取范围见 [版本记录](../third_party/libvterm/VERSION.md)，逐文件摘要见 [SHA256SUMS](../third_party/libvterm/SHA256SUMS)，许可见 [LICENSE](../third_party/libvterm/LICENSE)。构建不联网，许可复制为 `licenses/libvterm-LICENSE.txt`。
该目录保留原始行尾及上游空白，不对其执行第一方格式修正；`.gitattributes` 固定原样入库，提交时核对逐文件摘要。
