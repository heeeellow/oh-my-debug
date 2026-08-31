# TestTools

TestTools 是面向 Rockchip 板卡研发、测试和现场支持的 Windows 调试工作台。上位机使用 Qt 6
提供界面，协议、串口和测试逻辑尽量保持为标准 C++；板端通过无 Qt Agent 提供受控设备能力。

当前产品版本以根目录 [`VERSION.txt`](VERSION.txt) 为准，发布标签使用 `v<主版本>.<次版本>.<修订号>`。

项目首先围绕本地/板端串口调试建立完整链路，再逐步承接板级测试。实际完成范围、验证证据和开放项
统一以 [`docs/STATUS.md`](docs/STATUS.md) 为准。

## 使用与开发入口

- 想了解当前能做什么：读 [当前状态](docs/STATUS.md) 和 [产品设计基线](docs/PRODUCT.md)。
- 想修改串口功能：读 [串口工作台功能契约](docs/features/SERIAL.md) 和
  [界面与交互设计](docs/UI_DESIGN.md)。
- 想修改分层或 Agent：读 [总体架构](docs/ARCHITECTURE.md) 和
  [Agent 协议 v1](docs/AGENT_PROTOCOL_V1.md)。
- 想构建、验证或新增功能：读 [开发手册](docs/DEVELOPMENT.md)。
- 想追溯历史问题：读 [开发日志](docs/DEVLOG.md)。
- 想核对依赖来源：读 [第三方依赖](docs/THIRD_PARTY.md)。

开发者或自动化编码工具开始工作前还应阅读根目录 [`AGENTS.md`](AGENTS.md)。它只定义接手顺序和
不可破坏的工程边界，不复制当前状态。

## 快速开始

在已配置 Qt VS Tools 的 Visual Studio 中打开根目录 `TestTools.slnx`，选择 `x64` 和
Debug/Release 构建。主解决方案只包含 Qt GUI，启动产物位于
`build/windows/x64/<Configuration>/TestTools.exe`。

源码集中在 `src/`，板端 Agent 位于 `src/agent/`，第三方依赖和生成物分别位于
`third_party/` 与 `build/`。详细环境与命令见开发手册。
