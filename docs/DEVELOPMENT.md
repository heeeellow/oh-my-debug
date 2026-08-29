# TestTools 开发手册

## 1. 当前环境

- Windows 10/11
- Visual Studio 2022 或兼容的 MSVC 工具集
- Qt 6.11.2 MSVC 64-bit
- C++17

Windows 上位机工程为根目录 `TestTools.vcxproj`；板端无 Qt Agent 位于 `src/agent/`，通过 CMake
面向 Android/Linux 独立构建。产品程序不再依赖或启动本机假 Agent。

## 2. 目录约定

```text
src/
├── agent/         无 Qt 的真实板端 Agent；假服务仅为可选测试夹具
├── application/   用例、设备会话和任务协调
├── core/          标准 C++ 核心、传输和平台无关逻辑
├── diagnostics/   内置自检和实机冒烟入口
├── ui/            Qt 界面与 GUI 线程桥接
└── main.cpp       Windows 应用入口
third_party/       已审查的第三方依赖
scripts/           构建和部署脚本
docs/              架构、开发手册与开发记录
build/             所有生成物，不提交 Git
```

目录随代码实际出现创建，不提前放空目录。

## 3. 强制边界

- `src/core/`、`src/application/` 不得包含 Qt 头文件。
- UI 不直接拼接 ADB shell 命令；通过核心接口调用。
- 后台线程不得直接访问 Qt 控件。
- 设备节点、测试 ID 和外部参数必须校验。
- 不提供任意 shell RPC。
- 新增长任务必须定义超时、取消、资源占用和关闭行为。

## 4. 构建

在已配置 Qt VS Tools 的 Visual Studio 中打开根目录 `TestTools.slnx`，选择 `x64` 和
Debug/Release 构建。

命令行构建时需要让 MSBuild 找到 QtMsBuild，并确保 Qt 安装已注册：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  '.\TestTools.vcxproj' /m /p:Configuration=Release /p:Platform=x64 `
  /p:QtMsBuild='C:\Users\26613\AppData\Local\QtMsBuild'
```

Visual Studio 构建主程序时，如果能从 `TestToolsNdkRoot`、`ANDROID_NDK_ROOT`、
`ANDROID_NDK_HOME` 或本机 Android SDK 目录找到 NDK，会自动构建 Agent 并复制到与主程序相同的
Debug/Release 运行目录。也可单独运行仓库脚本；以当前 `armeabi-v7a` 板卡为例：

```powershell
.\scripts\build-android-agent.ps1 `
  -NdkRoot 'C:\Android\Sdk\ndk\27.3.13750724' `
  -CMakeProgram 'cmake' -NinjaProgram 'ninja'
```

主程序产物位置为 `build/windows/x64/Release/TestTools.exe`，对应 Android Agent 位于
`build/windows/x64/Release/agents/android/armeabi-v7a/testtools-agent`。所有构建目录和二进制
均不提交 Git；上位机根据 `ro.product.cpu.abi` 选择对应 Agent。

## 5. 验证层级

- 核心静态检查：依赖方向、输入校验、线程关闭和错误边界。
- `--self-test`：编码、异步发送队列、串口可靠性帧、Agent 帧、ADB 枚举和安全转发冒烟检查。
- `--agent-smoke <host> <port>`：验证真实或测试 Agent 的 hello 与能力发现。
- `--agent-deploy-smoke <adb-serial>`：执行真实 ABI 识别、部署、启动、控制转发、`system.info`
  和 `serial.list`；不发送串口数据。
- `--agent-reconnect-smoke <adb-serial>`：不重新部署，探测并连接板端现有 Agent，验证握手和转发。
- `--agent-at-smoke <adb-serial> <endpoint>`：通过与界面相同的 Agent 串口传输发送一次安全的
  `AT\r\n`，并验证在 5 秒内收到 `OK`；该检查会真实发送 AT 指令。
- `--agent-serial-smoke <adb-serial> <control-port> <endpoint>`：经真实 Agent 打开指定串口并保持
  5 秒，验证数据通道不会立即 EOF；不发送任何字节。
- `--adb-serial-smoke` 仅保留用于复现旧固件 ADB 设备节点转发问题，不是产品串口路径。
- `--ui-snapshot <png>`：保存概览页快照；可加 `--ui-page serial`、`tests`、`history`
  或 `settings` 选择待验收页面，并用 `--ui-size 1366x768` 指定逻辑窗口尺寸。
- UI 快照：检查主要分辨率下的裁切、重叠和状态层级。
- 实机验证：串口连接、收发、双向验证和异常断开。
- Agent 阶段：协议单元测试、真实板端部署、能力枚举、稳定保持，再进行已知安全指令的收发测试。

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
