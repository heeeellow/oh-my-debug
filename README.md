# TestTools

TestTools 是面向 Rockchip 板卡调试与测试的 Windows 工作台。当前可用能力是双端点串口调试，
后续将通过板端 Agent 扩展 DDR、存储、音频、网络、GPIO、重启/休眠等测试项。

## 文档入口

- [总体架构](docs/ARCHITECTURE.md)
- [界面与交互设计](docs/UI_DESIGN.md)
- [开发手册](docs/DEVELOPMENT.md)
- [开发日志](docs/DEVLOG.md)
- [第三方依赖与来源](docs/THIRD_PARTY.md)

当前 Qt 工程位于 `test/`，使用 Qt 6、C++17 和 Visual Studio 构建。
