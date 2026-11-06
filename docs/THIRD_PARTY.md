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
| iperf3 与 Windows Cygwin 运行时 | [ESnet 3.20](https://github.com/esnet/iperf/releases/tag/3.20) · [固定 Windows 构建](https://github.com/ar51an/iperf3-win-builds/releases/tag/3.20) | iperf3 `3.20`；随该 Windows 产物的 Cygwin `3.6.5-1` | 原始源码包不改写，增加本地 CMake 特征探测；三目标 Runner 与 Windows 工具离线分发；许可、来源及待核对项见 [版本记录](../third_party/iperf3/VERSION.md) |
| nlohmann/json | [固定发布](https://github.com/nlohmann/json/releases/tag/v3.11.3) | `3.11.3`（MIT） | 原始单头文件；仅适配 iperf3 的小数输出，控制协议继续使用原有严格 JSON 模型 |
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

工具箱依赖的固定文件见 [iperf3 摘要](../third_party/iperf3/SHA256SUMS.txt) 与 [JSON 摘要](../third_party/nlohmann/SHA256SUMS.txt)，构建前由 `scripts/validate-toolbox-dependencies.ps1` 校验。
Cygwin 3.6.5 的官方源码与 3.6.5-1 构建配方已核对，发布包在 `licenses/iperf3/` 同时携带对应源码 ZIP；打包前核对其摘要和实际 DLL。
完整修订、许可文本核对与未重编译 DLL 的边界见 [对应源码说明](../third_party/iperf3/CYGWIN-SOURCE.md)。

## 源码裁剪与许可

第一方许可由根目录 [LICENSE](../LICENSE) 定义，不能覆盖第三方组件的既有权利。
CSerialPort 使用 LGPLv3 及上游链接例外，Asio 使用 Boost Software License 1.0，
tiny-process-library、libvterm 和 Phosphor Icons 使用 MIT；许可原文均保留。

Asio、CSerialPort 和 tiny-process-library 只保留项目构建使用的头文件、实现和许可；
未使用的示例、绑定、上游 CI、测试和生成配置不进入仓库。
三者各自的 `VERSION.md` 记录上游提交、裁剪范围和本地适配，`SHA256SUMS` 记录保留文件。
保留文件以整理前的 Git 内容为准，本地适配和许可均保留；行尾与摘要固定，避免不同检出环境造成校验差异。
不要对这些目录批量格式化。

Qt 由开发环境提供，当前使用 6.11.2 MSVC x64 动态库（Core、Gui、Widgets、Network、Svg 及插件），
不把 SDK 纳入 Git。发布脚本从同版本 Qt 源码收集 LGPL/GPL 文本和相关模块内第三方声明，
随 `licenses/Qt/` 提供；库保持动态可替换。对应源码来源为
[Qt 6.11.2 官方归档](https://download.qt.io/archive/qt/6.11/6.11.2/single/)。
MSVC 运行库使用 Visual Studio Redist 目录的 x64 CRT 原始 DLL，随主程序本地部署；不从 System32 或 SDK 拷贝文件，不携带 Debug 运行库。
包内清单记录运行库版本与逐文件摘要，来源和部署依据见 [Microsoft 部署说明](https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files)。

预览包包含依赖版本、摘要和许可证，公开分发前仍需按实际包复核对应源码提供方式及各项义务；
本页是依赖记录，不代替逐项分发审查。打包方式见 [开发手册](DEVELOPMENT.md#5-打包与公开前检查)。
