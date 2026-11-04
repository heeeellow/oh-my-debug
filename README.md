# Oh My Debug

Windows 上的板卡调试工具，支持本地串口、串口终端、ADB 文件传输、Android 投屏和设备维护。
原项目名为 TestTools，仓库标识使用 `oh-my-debug`。

- **串口调试**：单/双端收发、文本与 HEX、快捷指令、计划发送、链路验证及持续录制。
- **串口终端**：原生字符网格、ANSI/VT、中文、搜索、自动高亮与外观设置。
- **ADB 工具**：独立 Shell、文件管理、投屏、日志导出与明确确认的维护操作。
- **板端服务**：通过 Agent 托管设备串口，读取内存、存储和健康信息。

## 使用

解压完整 Windows 发布包后运行 `OhMyDebug.exe`，不要只拷贝主程序。
首次使用见 [使用说明](docs/USAGE.md)，当前完成度与验证范围见 [项目状态](docs/STATUS.md)。

## 开发

使用 Visual Studio 和 Qt VS Tools 打开 `OhMyDebug.slnx`，选择 `x64 / Release`。
环境、命令行构建、测试与打包见 [开发手册](docs/DEVELOPMENT.md)。

| 目录 | 内容 |
| --- | --- |
| `src/` | 桌面程序、板端 Agent 及分层实现 |
| `tests/` | 无设备自动检查、界面检查与独立手工工具 |
| `third_party/` | 固定版本的依赖源码、工具及许可 |
| `scripts/` | 构建、验证与打包 |
| `docs/` | 使用说明、功能契约、架构和开发记录 |
| `build/` | 本地产物与证据，不纳入 Git |

开发入口为 [AGENTS.md](AGENTS.md)，分层与线程规则见 [架构说明](docs/ARCHITECTURE.md)。
本项目的开发记录只维护在仓库中。

## 许可

Copyright (c) 2026 Yanzhaohui。第一方代码采用 [限制用途的源码可见许可](LICENSE)：
允许阅读和个人学习研究，商用、修改版分发须另行取得书面授权。
这不是 MIT 等开放源代码许可。第三方组件继续适用各自许可证，见 [第三方依赖](docs/THIRD_PARTY.md)。
