# TestTools

TestTools 是面向 Rockchip 板卡与通用硬件研发、测试和现场排障的轻量级 Windows 调试工作台。产品围绕**串口调试**（单串口收发、双端链路验证、Debug 串口终端）、**ADB 文件传输与远程调试（终端、Android 投屏）**和**设备连接诊断**组织工作流，保持界面克制、开箱即用。

当前版本见根目录 [`VERSION.txt`](VERSION.txt)，已实现范围、验证状态和开放风险统一见 [`docs/STATUS.md`](docs/STATUS.md)。

## 产品与规划入口

- [产品设计](docs/PRODUCT.md)：使用者、核心任务边界与首版范围。
- [界面设计](docs/UI_DESIGN.md)：页面结构、文案、视觉规范与适配要求。
- [串口调试契约](docs/features/SERIAL.md)：连接、收发、快捷指令、计划发送与可靠性压测规则。
- [串口终端契约](docs/features/SERIAL_TERMINAL.md)：Debug 控制台、原生字符网格、复制粘贴、连续输出与 VT 解析。
- [ADB 终端契约](docs/features/ADB_TERMINAL.md)：内置 ADB、当前设备 Shell 与系统终端回退规则。
- [文件传输契约](docs/features/FILE_TRANSFER.md)：设备目录书签与收藏、文件/文件夹收发、受控文件管理及任务取消规则。
- [Android 投屏契约](docs/features/SCREEN_MIRROR.md)：独立投屏窗口、设备控制、截图与进程生命周期。
- [设备诊断与维护契约](docs/features/DEVICE_MAINTENANCE.md)：日志导出、Root/Remount、重启、固定设备与任务互斥。
- [板卡硬件信息契约](docs/features/BOARD_INFO.md)：内存、存储和健康字段的只读展示、顺序读取与会话隔离，不做规格比较。
- [前置清理、原生感终端、GPIO 与 iperf3 工具规划（ADR-0007）](docs/decisions/0007-native-terminal-gpio-and-iperf3-plan.md)：阶段 P0 仓库清理与终端架构解耦、`libvterm` 字符网格终端、内嵌 ADB Shell、GPIO 快速控制与 iperf3 离线打包测速方案。
- [工作台交互升级与板级能力规划（ADR-0006）](docs/decisions/0006-workstation-ux-and-capabilities-plan.md)：顶栏重塑、快捷指令右侧栏化、串口流式录制、ADB 文件与维护增强及分阶段实施计划。
- [整体迭代与新会话接手（ADR-0005）](docs/decisions/0005-adb-workspace-terminal-and-files.md)：ADB 工作区、导航与顶栏、独立文件传输页、后续终端候选及投屏音频边界；区分已接受方向和待定项。
- [上一轮路线图（ADR-0004）](docs/decisions/0004-serial-terminal-and-adb-integration.md)：Debug 串口终端、内置 ADB 打包与串口补强方案。

## 开发与架构入口

- [总体架构](docs/ARCHITECTURE.md) 与 [设备协议](docs/AGENT_PROTOCOL_V1.md)：分层边界、连接模型与板端 Agent 能力。
- [开发手册](docs/DEVELOPMENT.md)：构建、验证、板端产物与新增模块要求。
- [当前状态](docs/STATUS.md)：当前工作树、最近验证结果与下一阶段任务。
- [开发日志](docs/DEVLOG.md)：仅用于追溯历史过程。
- [第三方依赖](docs/THIRD_PARTY.md)：依赖来源与许可证。

开发者和自动化编码工具开始工作前必须阅读 [`AGENTS.md`](AGENTS.md)。项目记录只维护在仓库 `docs/` 中，不接入外部知识库。

## 构建

在已配置 Qt VS Tools 的 Visual Studio 中打开 `TestTools.slnx`，选择 `x64` 和 Debug 或 Release。
主程序位于 `build/windows/x64/<Configuration>/TestTools.exe`。详细环境、命令和设备侧产物规则见 [开发手册](docs/DEVELOPMENT.md)。

解决方案内按应用层、核心层、适配器和界面职责浏览源码，另提供文档、脚本、测试及板端 Agent 入口。
新增文件后用 `scripts/sync-vs-layout.ps1` 同步分组，维护规则见 [VS 解决方案视图](docs/DEVELOPMENT.md#21-visual-studio-解决方案视图)。
