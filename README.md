# Oh My Debug

Windows 上的串口和板卡调试工具。可以通过 USB 转串口查看启动日志、操作 Debug Shell，
也可以通过 ADB 管理 Android 或 Linux 设备的文件、打开 Shell、导出日志。
Android 投屏和设备串口托管有各自的入口。

原项目名为 TestTools；程序显示名称为 Oh My Debug，源码仓库目前仍使用 `test-tools` 名称。

## 能做什么

| 功能 | 用途 |
| --- | --- |
| 单串口调试 | 文本/HEX 收发、快捷指令、收发计数、定时或按次数发送 |
| 双端调试 | 同时观察两个串口，进行双向链路验证和可靠性测试 |
| 串口终端 | 查看连续启动日志、进入 Debug Shell，支持 ANSI/VT、中文、搜索和复制粘贴 |
| 持续录制 | 将收发数据保存到文件，录制不受显示清屏和历史裁剪影响 |
| ADB 文件管理 | 双栏浏览、文件和文件夹上传下载、新建、改名、删除及权限编辑 |
| ADB Shell | 为选中的设备打开独立系统终端 |
| Android 投屏 | 查看和控制设备屏幕、保存截图；当前不启用音频转发 |
| 设备维护与信息 | 导出 Logcat/dmesg、执行 Root/Remount 或重启，读取设备内存与存储信息 |

## 运行环境和设备范围

桌面端目前提供 **Windows x64** 成品，日常使用选择 Release。
当前构建和界面验证在 Windows 11 完成，Windows 10 的兼容性仍需单独验证。
暂未提供 Linux、macOS 或 Windows ARM64 的桌面成品。

| 设备或连接方式 | 可以使用的部分 | 条件 |
| --- | --- | --- |
| Windows 能识别的 COM 串口 | 本地串口收发、终端、录制和双端调试 | 安装串口设备驱动，串口参数与设备一致；设备运行什么系统不影响本地串口连接 |
| 启用 ADB 的 Android 设备 | 文件、Shell、日志导出和投屏 | USB 调试已启用并授权；具体操作受系统版本、权限和固件限制 |
| Android ARMv7 / ARM64 | 设备串口、板卡硬件信息 | 随包板端服务提供 `armeabi-v7a`、`arm64-v8a` 产物，构建最低 API 为 23；还需设备允许启动服务和访问相应节点 |
| Linux ARM64，glibc 2.31 及以上 | 设备串口、板卡硬件信息 | 固件带 adbd，自动部署要求 root adbd，能够写入 `/opt`、`/run` 和 `/var/log` |
| 其他带 adbd 的 Linux 固件 | ADB Shell 和文件操作 | 取决于固件提供的命令与权限；随包 Agent 不覆盖所有架构和 libc |

Linux 板端服务当前只提供 `aarch64/glibc-2.31` 目标，未提供 Linux ARM32、x86、musl 或 uClibc 的现成包。
Buildroot、Yocto 等系统需要核对实际架构、libc 和 adbd 配置，不能只按发行版名称判断。
没有 ADB 的设备仍可通过 PC 端串口调试；当前没有 SSH 连接板卡的入口。

实机记录主要来自 **RK3568 / Android 11**，不表示其他 Rockchip 型号或所有 Android 固件都已逐项验证。
各版本实际测过的功能和仍待验证的内容见 [当前状态](docs/STATUS.md)。

## 第一次使用

1. 解压完整 Windows 成品 ZIP，运行 `OhMyDebug.exe`。只下载源码包无法直接启动。
2. 查看串口收发数据，进入“串口调试 → 单串口调试”；进入开发板 Debug Shell，选择“串口终端”。
3. 使用 ADB 时，通过 USB 连接设备，在设备上确认调试授权，再从顶栏选择在线设备。
4. 只有设备串口和板卡硬件信息需要“连接服务”；本地 COM、ADB 文件、Shell 和投屏可以分别使用。

顶部的设备列表用于选择 **ADB 设备**，串口页面里的下拉框用于选择 **COM 口或设备串口**。
ADB 未连接时，本地 COM 串口仍然可用。

详细步骤见 [使用手册](docs/USAGE.md)，包含第一次连接、日志保存、文件收发、投屏和常见问题。

## 成品包与设置

请保留程序旁的 Qt DLL、`platforms/`、`platform-tools/`、`scrcpy/` 和 `agent-bundle/`，不要只复制 EXE。
若 Windows 提示缺少 MSVC 运行库，可安装包内的 `vc_redist.x64.exe`。

连接参数和界面偏好保存在 `%LOCALAPPDATA%/TestTools/`，改名后继续兼容原设置。
收发日志需要手动保存或开启录制，不会仅因连接成功就自动保存完整会话。

## 开发

桌面使用 C++17、Qt 和 Visual Studio；板端服务使用独立 CMake 和 Android/Linux 工具链。
使用已配置 Qt VS Tools 的 Visual Studio 打开 `OhMyDebug.slnx`，选择 `x64 / Release`。
解决方案中的 `agent` 文件夹提供板端源码、协议、测试和构建脚本入口。

| 路径 | 内容 |
| --- | --- |
| `src/` | 桌面、板端服务及共享协议实现 |
| `tests/` | 核心与界面检查、独立手工工具 |
| `third_party/` | 固定版本的依赖源码、工具和许可 |
| `scripts/` | 构建、验证与打包 |
| `docs/` | 使用、设计、开发和验证记录 |
| `build/` | 本地产物，不纳入 Git |

- [开发手册](docs/DEVELOPMENT.md)：环境配置、桌面与 Agent 构建、检查和打包。
- [架构说明](docs/ARCHITECTURE.md)：分层、线程与数据流。
- [第三方依赖](docs/THIRD_PARTY.md)：依赖来源、版本、本地修改和许可证。

参与开发前阅读 [AGENTS.md](AGENTS.md)。项目记录统一维护在本仓库的 `docs/` 中。

## 许可

Copyright (c) 2026 Yanzhaohui。第一方代码采用 [限制用途的源码可见许可](LICENSE)：
允许阅读和个人学习研究，商用、修改版分发须另行取得书面授权。
第三方组件保留各自的版权和许可证，详见 [第三方依赖](docs/THIRD_PARTY.md)。
