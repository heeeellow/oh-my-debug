# TestTools

TestTools 是面向 Rockchip 板卡与通用硬件研发、测试和现场排障的轻量级 Windows 调试工作台。产品核心围绕**串口调试**（单串口收发、双端链路验证、Debug 串口终端）与**设备连接诊断**组织工作流，保持界面克制、开箱即用。

当前版本见根目录 [`VERSION.txt`](VERSION.txt)，已实现范围、验证状态和开放风险统一见 [`docs/STATUS.md`](docs/STATUS.md)。

## 产品与规划入口

- [产品设计](docs/PRODUCT.md)：使用者、核心任务边界与首版范围。
- [界面设计](docs/UI_DESIGN.md)：页面结构、文案、视觉规范与适配要求。
- [串口调试契约](docs/features/SERIAL.md)：连接、收发、快捷指令、计划发送与可靠性压测规则。
- [ADB 终端契约](docs/features/ADB_TERMINAL.md)：内置 ADB、当前设备 Shell 与系统终端回退规则。
- [迭代路线图（ADR-0004）](docs/decisions/0004-serial-terminal-and-adb-integration.md)：Debug 串口终端、内置 ADB 打包与串口补强方案。

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
