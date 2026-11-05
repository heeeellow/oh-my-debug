# Oh My Debug

用于开发板日常调试的 Windows 工具，集成串口收发、Debug 终端、ADB 文件管理、Android 投屏和测试工具箱。
适合查看启动日志、调试串口协议、传送文件，以及通过 Shell 检查设备状态。

[使用手册](docs/USAGE.md) · [开发手册](docs/DEVELOPMENT.md) · [项目状态](docs/STATUS.md) · [0.18.1 更新说明](docs/releases/0.18.1.md)

## 功能

- **串口收发**：文本与 HEX、快捷指令、定时或按次数发送，支持单串口和双端调试。
- **串口终端**：ANSI/VT、中文显示、搜索、复制粘贴、错误与警告自动高亮，可调整字体和配色。
- **日志记录**：保存当前记录，或持续录制文本、原始接收字节。
- **ADB 调试**：文件和文件夹上传下载、独立 Shell、Logcat/dmesg 导出、Root/Remount 和重启。
- **Android 投屏**：查看和控制设备屏幕，保存截图。
- **板端服务**：操作开发板上的串口，读取系统、内存和存储信息。
- **工具箱**：GPIO 管脚号双向换算、占用查看、读取、输出保持、单次脉冲与输入监听；自动使用已适配的设备控制接口读取与设置；指定以太网接口进行 TCP/UDP 单向或顺序双向测速，分别显示双向曲线并导出报告。

## 支持环境

桌面程序运行于 **Windows 10（1809 起）/ Windows 11 x64**。设备可以通过电脑串口或 USB ADB 连接：

| 连接方式 | 适用设备 | 准备工作 |
| --- | --- | --- |
| 本地 COM 串口 | USB 转串口、MCU、开发板 Debug 口，以及 U-Boot、Linux、Android 串口控制台 | 安装 Windows 串口驱动，设置与设备一致的波特率等参数 |
| Android ADB | 开启 USB 调试的 Android 设备 | 安装 ADB 驱动，在设备上确认调试授权 |
| Linux ADB | 固件中运行 adbd 的 Linux 开发板 | 启动 adbd，通过 USB 连接电脑 |

本地串口可独立使用。ADB 文件管理和 Shell 适用于 Android、Linux；投屏使用 scrcpy，面向 Android。

使用设备串口、板卡信息和工具箱时，程序会部署随包的板端服务。目前提供以下版本：

| 系统 | 板端服务目标 |
| --- | --- |
| Android | ARMv7、ARM64，API 23 起 |
| Linux | ARM64，glibc 2.31 起 |

Linux 自动部署使用 root adbd，并需要 `/opt`、`/run`、`/var/log` 的写入权限。
自定义固件请核对 CPU 架构、libc 和 adbd 配置。
GPIO 需内核字符设备接口及访问权限；设备提供的扩展控制接口需匹配支持的驱动与节点格式，详见[工具箱契约](docs/features/TOOLBOX.md)。网络测速需要两端可用的以太网接口和 IPv4 地址。
构建支持与实机验证分开记录，见 [项目状态](docs/STATUS.md)。

## 开始使用

解压完整的 Windows 成品包，运行 `OhMyDebug.exe`。保留包内目录结构，程序会使用其中的运行库和配套工具。
便携包同时提供 Qt、MSVC 运行库和配套工具，无需安装开发环境或配置 PATH；请勿只复制主程序。

- **收发串口数据**：进入“串口调试 → 单串口调试”，选择 COM 口和波特率，点击“连接”。
- **使用 Debug Shell**：进入“串口调试 → 串口终端”，连接后直接在终端中输入。
- **使用 ADB**：连接 USB 并完成授权，在顶部设备栏选择设备，再进入“ADB 调试”。
- **使用设备串口或查看板卡信息**：选中 ADB 设备后，点击顶部“连接服务”。
- **使用测试工具**：连接服务后进入“工具箱 → GPIO 调试 / 网络测速”，选择引脚或两端网口后操作。

首次连接、日志录制、文件收发和常见问题的详细步骤见 [使用手册](docs/USAGE.md)。

## 编译与开发

桌面程序使用 C++17、Qt 6.11.2 和 Visual Studio。配置好 Qt VS Tools 后，打开 `OhMyDebug.slnx`，
选择 `x64 / Release` 编译运行。板端服务位于 `src/agent/`，使用独立 CMake 工程交叉编译；
解决方案中的 `agent` 文件夹提供源码和构建脚本入口。

- [开发手册](docs/DEVELOPMENT.md)：环境配置、桌面与板端构建、测试和打包。
- [架构说明](docs/ARCHITECTURE.md)：源码目录、模块职责和线程模型。
- [第三方依赖](docs/THIRD_PARTY.md)：组件版本、来源、本地修改和许可证。
- [开发约定](AGENTS.md)：仓库维护规则。

## 许可

Copyright (c) 2026 Yanzhaohui。

项目源码允许阅读和个人学习研究，商用、修改版分发需另行取得书面授权，完整条款见 [LICENSE](LICENSE)。
第三方组件按各自许可证使用，详见 [第三方依赖](docs/THIRD_PARTY.md)。
