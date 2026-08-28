# TestTools 开发手册

## 1. 当前环境

- Windows 10/11
- Visual Studio 2022 或兼容的 MSVC 工具集
- Qt 6.11.2 MSVC 64-bit
- C++17

当前工程入口为 `test/test.slnx`，项目文件为 `test/test.vcxproj`。

## 2. 目录约定

```text
test/
├── core/          标准 C++ 核心、传输和平台无关逻辑
├── application/   用例、设备会话和任务协调
├── ui/            Qt 界面
│   ├── adapters/  标准 C++ 与 Qt 的桥接
│   ├── pages/     功能页面
│   └── widgets/   本项目通用控件
├── third_party/   已审查的第三方依赖
└── main.cpp
```

目录随代码实际出现创建，不提前放空目录。

## 3. 强制边界

- `core/`、`application/` 不得包含 Qt 头文件。
- UI 不直接拼接 ADB shell 命令；通过核心接口调用。
- 后台线程不得直接访问 Qt 控件。
- 设备节点、测试 ID 和外部参数必须校验。
- 不提供任意 shell RPC。
- 新增长任务必须定义超时、取消、资源占用和关闭行为。

## 4. 构建

在已配置 Qt VS Tools 的 Visual Studio 中打开 `test/test.slnx`，选择 `x64` 和 Debug/Release 构建。

命令行构建时需要让 MSBuild 找到 QtMsBuild，并确保 Qt 安装已注册：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  '.\test\test.vcxproj' /m /p:Configuration=Release /p:Platform=x64 `
  /p:QtMsBuild='C:\Users\26613\AppData\Local\QtMsBuild'
```

## 5. 验证层级

- 核心静态检查：依赖方向、输入校验、线程关闭和错误边界。
- `--self-test`：编码、ADB 枚举和安全转发冒烟检查。
- `--ui-snapshot <png>`：保存概览页快照；可加 `--ui-page serial`、`tests`、`history`
  或 `settings` 选择待验收页面，并用 `--ui-size 1366x768` 指定逻辑窗口尺寸。
- UI 快照：检查主要分辨率下的裁切、重叠和状态层级。
- 实机验证：串口连接、收发、双向验证和异常断开。
- Agent 阶段：协议单元测试、假服务测试，再进入真实板端。

构建通过不等于实机通过；验证记录写入 `DEVLOG.md`。

## 6. 新增功能

1. 先在架构中确认能力属于上位机、Agent 还是测试执行程序。
2. 定义稳定能力 ID、参数、结果、资源和风险。
3. 为简单能力使用通用页；复杂交互再创建专用页面。
4. 核心逻辑先通过非 Qt 测试，再连接 Qt 适配器。
5. 验证取消、超时、断线和退出，不只验证成功路径。

## 7. 文档职责

- `ARCHITECTURE.md`：稳定分层、协议和设计决策。
- `UI_DESIGN.md`：用户任务、布局与交互规则。
- `DEVELOPMENT.md`：如何构建、扩展和验证。
- `DEVLOG.md`：实际完成的里程碑、证据和剩余边界。

设计变化先更新权威文档，不在多个文件复制同一份当前状态。
