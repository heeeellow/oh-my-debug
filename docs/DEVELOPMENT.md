# TestTools 开发手册

[返回项目首页](../README.md)

## 1. 当前环境

- Windows 10/11
- Visual Studio 2022 或兼容的 MSVC 工具集
- Qt 6.11.2 MSVC 64-bit
- C++17

Windows 上位机工程为根目录 `TestTools.vcxproj`；板端无 Qt Agent 位于 `src/agent/`，通过 CMake
面向 ARM Android/Linux 独立构建。产品程序不依赖协议测试夹具。

主解决方案 `TestTools.slnx` 只包含 Qt GUI 工程，确保 Visual Studio 的“本地 Windows 调试器”
始终启动界面程序。用于协议开发的 Windows Agent 工程保留在
`src/agent/TestToolsAgent.vcxproj`，需要时单独打开或命令行构建，不作为产品启动项目。

## 2. 目录约定

```text
src/
├── adapters/      ADB、Agent TCP 和 Windows COM 等具体适配器
├── agent/         无 Qt 的真实板端 Agent；本机服务仅为可选协议测试夹具
├── application/   按 agent/device/serial/support 组织的用例和生命周期
├── core/          按 protocol/serial/support 组织的平台无关逻辑
├── diagnostics/   内置自检和实机冒烟入口
├── ui/            Qt 界面与 GUI 线程桥接
└── main.cpp       Windows 应用入口
third_party/       已审查的第三方依赖
scripts/           构建和部署脚本
docs/              架构、开发手册与开发记录
build/             所有生成物，不提交 Git
```

目录随代码实际出现创建，不提前放空目录。

仓库根目录 `AGENTS.md` 是工具无关的接手入口；`docs/features/` 保存功能行为契约，
`docs/decisions/` 只保存影响长期维护的重要决策，不为每次修改创建文档。

## 3. 强制边界

- `src/core/`、`src/application/`、`src/adapters/` 不得包含 Qt 头文件。
- UI 不直接拼接 ADB shell 命令；通过核心接口调用。
- 后台线程不得直接访问 Qt 控件。
- 设备节点、测试 ID 和外部参数必须校验。
- 不提供任意 shell RPC。
- 新增长任务必须定义超时、取消、资源占用和关闭行为。
- UI 不直接包含 `src/adapters/` 中的具体串口传输；具体适配器通过 application 服务创建。

仓库使用 `.editorconfig`、`.gitattributes` 和 `.clang-format` 固定 UTF-8、LF、缩进和 C++ 基础
格式。格式化只作用于本次修改的第一方文件，不批量改写第三方依赖或无关用户改动。

## 4. 构建

在已配置 Qt VS Tools 的 Visual Studio 中打开根目录 `TestTools.slnx`，选择 `x64` 和
Debug/Release 构建。

命令行构建建议在 Visual Studio Developer PowerShell 中运行，并确保 Qt 安装已注册。不要把个人
账户目录写入共享脚本；当前机器可使用：

```powershell
$qtMsBuild = Join-Path $env:LOCALAPPDATA 'QtMsBuild'
msbuild '.\TestTools.vcxproj' /m /p:Configuration=Release /p:Platform=x64 `
  /p:QtMsBuild="$qtMsBuild"
```

如果 Developer PowerShell 中找不到 `msbuild`，应通过 Visual Studio Installer 或 `vswhere` 定位
实际安装，不在项目文档中假定固定版本号和安装盘符。

### 4.1 Android ARM Agent

Visual Studio 构建主程序时，如果能从 `TestToolsNdkRoot`、`ANDROID_NDK_ROOT`、
`ANDROID_NDK_HOME` 或本机 Android SDK 目录找到 NDK，会自动构建 Agent 并复制到与主程序相同的
Debug/Release 运行目录。默认目标为 `armeabi-v7a`，可通过 MSBuild 属性
`TestToolsAndroidAgentAbi=arm64-v8a` 切换。也可单独运行仓库脚本：

```powershell
.\scripts\build-android-agent.ps1 `
  -NdkRoot 'C:\Android\Sdk\ndk\27.3.13750724' `
  -Abi armeabi-v7a `
  -CMakeProgram 'cmake' -NinjaProgram 'ninja'
```

`-Abi` 只接受 `armeabi-v7a` 和 `arm64-v8a`。主程序产物位于
`build/windows/x64/Release/TestTools.exe`，Agent 位于同目录的
`agents/android/<abi>/testtools-agent`。上位机根据 `ro.product.cpu.abi` 选择产物。

Android 产物使用 NDK/Bionic，最低 API 为 23。同一 ABI 可以覆盖 Android 11、13、14 等更高版本；
更换系统版本后仍需重新验证 SELinux、设备节点权限和系统服务占用。

### 4.2 Linux ARM Agent

Ubuntu 或其他 Linux 板卡使用 glibc 产物，不能复用 Android 二进制。在 ARM 板端取得仓库后可原生
构建并运行主机契约测试：

```bash
bash ./scripts/build-linux-agent.sh Release
```

脚本只接受 `armv7/armv7l/armhf` 或 `aarch64/arm64` 主机，产物位于
`build/agents/linux/<armv7|aarch64>/testtools-agent`。Windows 上已有 Linux ARM 交叉工具链时可运行：

```powershell
.\scripts\build-linux-agent.ps1 `
  -ToolchainFile 'C:\toolchains\linux-aarch64.cmake' `
  -Architecture aarch64
```

交叉工具链文件必须指定编译器和与目标系统兼容的 sysroot；交叉构建不代替板端契约测试。当前 GUI
只实现 Android ADB 自动部署，Linux 产物需要手动启动；SSH 部署、systemd 服务和端口隧道仍是后续
连接器工作。

Kernel 5.10 与 6.1 通常共享同一用户空间产物。切换内核时重点回归串口驱动、节点名称、termios、
`/proc`/`/sys` 可见性和权限策略，而不是为每个内核建立一套源码。

所有构建目录和二进制均不提交 Git。

## 5. 验证层级

日常修改先运行统一静态入口：

```powershell
.\scripts\check-baseline.ps1
```

交付前可由该入口定位 MSBuild，构建 Debug/Release，并运行无设备依赖的 Agent 协议契约测试：

```powershell
.\scripts\check-baseline.ps1 -Build -Configuration All
```

`-RunSelfTest` 会执行需要在线 ADB 设备的安全自检，不应在无设备环境中默认启用。构建目标正在运行
时链接器无法覆盖程序，应先正常关闭 TestTools，而不是强制终止未知用户进程。

- 核心静态检查：依赖方向、输入校验、线程关闭和错误边界。
- Core/Application 主机契约测试：由 `tests/CMakeLists.txt` 构建，不依赖 Qt、ADB 或串口设备，覆盖
  编解码、发送计划、异步队列、串口会话代次与关闭、工作区状态、资源冲突和后台任务所有权。
- Agent 主机契约测试：由 CTest 验证协议版本、字段类型、枚举和范围拒绝路径，不访问真实设备。
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
  或 `settings` 选择待验收页面，并用 `--ui-size 1366x768` 指定逻辑窗口尺寸。串口页可再加
  `--ui-serial-mode single|dual` 验收两种调试方式；可靠性压测位于双端调试中。
- UI 快照：检查主要分辨率和 DPI 下的裁切、重叠、文字清晰度、状态层级和主动作。
- 实机验证：串口连接、收发、双向验证和异常断开。
- Agent 阶段：协议单元测试、真实板端部署、能力枚举、稳定保持，再进行已知安全指令的收发测试。

构建通过不等于实机通过。当前验证摘要更新到 `STATUS.md`；只有会影响后续判断的里程碑和决定性
证据才写入 `DEVLOG.md`，避免把每次命令输出变成永久文档。

## 6. 新增功能

1. 在 `PRODUCT.md` 或对应 `features/` 契约中确认用户任务、非目标、状态和验收条件。
2. 在架构中确认能力属于 Qt UI、application、core、传输适配器、Agent 或测试执行程序。
3. 板端能力定义稳定 ID、参数、结果、资源、风险、超时和取消语义。
4. 核心状态和数据转换先通过无 Qt 测试，再连接 Qt 适配器。
5. 简单参数能力使用通用页；复杂交互只有在真实需求出现时创建专用页面。
6. 验证成功、取消、超时、断线、退出、权限和资源冲突，不用单一路径代替回归。
7. 行为、架构或当前状态变化时，在同一工作批次更新对应权威文档。

## 7. 文档职责

- `README.md`：稳定项目入口和按目的分流，不保存动态开放项。
- `AGENTS.md`：开发接手顺序和不可破坏边界，不复制项目状态。
- `STATUS.md`：当前源码、验证、交付、债务和下一阶段的唯一汇总。
- `PRODUCT.md`：用户、任务、术语和稳定产品规则。
- `features/*.md`：单项功能的行为、状态、分层归属和验收契约。
- `ARCHITECTURE.md`：稳定分层、依赖、协议和演进边界，不保存阶段进度。
- `UI_DESIGN.md`：视觉 token、布局、组件、文案和可访问性规则。
- `decisions/*.md`：少量重要决策及取舍，不复制完整功能说明。
- `DEVELOPMENT.md`：构建、扩展和验证方法。
- `DEVLOG.md`：带日期的里程碑和决定性历史证据，不承担当前状态。

同一事实只保留一个权威来源，其他文档保留必要上下文并链接。完成文档结构或链接变更后运行：

```powershell
.\scripts\check-project-docs.ps1
```

脚本通过只证明必要入口和相对链接有效，不能替代内容审查、构建或实机验证。

## 8. 版本与标签

根目录 `VERSION.txt` 保存 TestTools 产品版本，采用语义化版本号。Git 发布标签使用带 `v` 前缀的同一
版本，例如 `VERSION.txt` 为 `0.1.0` 时标签为 `v0.1.0`。产品版本与 Agent 程序版本独立演进，兼容性
由 Agent 握手中的协议范围和程序版本判断。

建立版本基线前更新 `VERSION.txt` 和 `STATUS.md`，完成规定验证后创建源码提交与附注标签。标签只表示
该提交的版本身份；是否推送、制作安装包或发布给使用者仍是独立交付动作。
