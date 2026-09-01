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
Debug/Release 构建。工程会通过 Qt VS Tools 的 `windeployqt` 自动把 Qt 运行时和必要插件复制到
`build/windows/x64/<Configuration>/`，该目录可直接启动 `TestTools.exe`；同时会复制存在的
`agent-bundle/`。

命令行构建建议在 Visual Studio Developer PowerShell 中运行，并确保 Qt 安装已注册。不要把个人
账户目录写入共享脚本；当前机器可使用：

```powershell
$qtMsBuild = Join-Path $env:LOCALAPPDATA 'QtMsBuild'
msbuild '.\TestTools.vcxproj' /m /p:Configuration=Release /p:Platform=x64 `
  /p:QtMsBuild="$qtMsBuild"
```

Android Agent 的 CMake/Ninja 也必须来自 PATH 或显式环境变量；例如在当前终端设置
`TESTTOOLS_CMAKE_PROGRAM`、`TESTTOOLS_NINJA_PROGRAM`，或直接向 Agent 构建脚本传入
`-CMakeProgram`、`-NinjaProgram`。工程不内置开发者机器的工具链绝对路径。

修改 `src/agent/`、Agent 协议行为或板端返回字段时，必须先递增
`src/core/protocol/agent_version.h` 中的 `AgentVersion`。这是上位机判断板端 Agent 是否需要
升级的唯一版本来源；以后新增或修改随包部署的 Runner/配置时也必须递增该版本，不再增加第二套
二进制指纹。启动后的后台发现只做只读握手；用户点击
“连接并同步 Agent”时，如果板端版本不同，上位机会自动推送并启动当前构建产物。

如果 Qt 未注册为工程中的 `QtInstall` 版本，先在 Qt VS Tools 中注册与工程配置一致的 Qt 安装；
不要把个人机器的 Qt 绝对路径写入工程或脚本。发布验收应在没有 Qt 环境变量的进程中直接启动上述
输出目录中的程序，并确认同目录包含 `Qt6Core.dll`、`Qt6Gui.dll` 和 `Qt6Widgets.dll`。

如果 Developer PowerShell 中找不到 `msbuild`，应通过 Visual Studio Installer 或 `vswhere` 定位
实际安装，不在项目文档中假定固定版本号和安装盘符。

### 4.1 Android ARM Agent

Visual Studio 构建主程序只复制已经生成的 `build/agent-bundle/`，不在 GUI 增量构建中隐式触发
Android 或 WSL 编译。复制前会验证 Bundle v2 的结构、文件完整性及其 AgentVersion 是否与当前
源码一致；缺少 Bundle 时给出警告，陈旧或损坏的 Bundle 会阻止复制。完整矩阵统一由
`build-agent-matrix.ps1` 生成。也可单独运行仓库脚本：

```powershell
.\scripts\build-android-agent.ps1 `
  -NdkRoot 'C:\Android\Sdk\ndk\27.3.13750724' `
  -Abi armeabi-v7a `
  -CMakeProgram 'cmake' -NinjaProgram 'ninja'
```

`-Abi` 只接受 `armeabi-v7a` 和 `arm64-v8a`。主程序产物位于
`build/windows/x64/Release/TestTools.exe`，Bundle 位于程序旁的
`agent-bundle/agents/android/<abi>/testtools-agent`。上位机通过 Bundle v2 根据
`ro.product.cpu.abi` 选择目标包。

Android 产物使用 NDK/Bionic，最低 API 为 23。同一 ABI 可以覆盖 Android 11、13、14 等更高版本；
更换系统版本后仍需重新验证 SELinux、设备节点权限和系统服务占用。

因此日常修改 Agent 后统一运行 4.2 节的 `build-agent-matrix.ps1`，再重建桌面程序以把完整 Bundle
复制到运行目录；不会再有单 ABI 的第二套来源。

### 4.2 Linux ARM Agent

固定构建矩阵的 Linux 目标只有 `aarch64`，使用 glibc 2.31 基线产物，不能复用 Android 二进制。
Linux 构建必须在 WSL2 中完成，CMake 构建目录位于 WSL 用户目录，不在 `/mnt/c` 下长期编译。
可信工具链由 Debian Bullseye 官方仓库的固定包和脚本内 SHA-256 校验值确定，安装到 WSL 用户目录，
不需要 `sudo`：

```powershell
wsl.exe -d Ubuntu-24.04 -- bash /mnt/c/path/to/test-tools/scripts/install-linux-aarch64-toolchain.sh
```

也可以把目标工具链目录作为脚本第一个参数传入。脚本固定使用 GCC 10.2.1 交叉编译器和 glibc
`2.31-9cross4` sysroot；如果校验或 sysroot 检查失败，必须停止构建。

Linux 板端的原生构建脚本仍可用于板端主机契约测试：

```bash
bash ./scripts/build-linux-agent.sh Release
```

该脚本只接受 `aarch64/arm64` 主机。固定 Bundle 不使用它，而使用统一矩阵入口。GUI 的 Linux
ADB 自动部署要求构建后将 `build/agent-bundle/` 复制到程序旁的 `agent-bundle/`；Visual Studio
工程在该 Bundle 存在时会自动执行复制：

```powershell
.\scripts\build-agent-matrix.ps1 `
  -NdkRoot '<Android NDK path>' `
  -NinjaProgram '<Ninja executable path>'
```

矩阵入口固定构建 Android `armeabi-v7a`、`arm64-v8a`（最低 API 23、`c++_static`）和 Linux
`aarch64/glibc-2.31`，最终产物位于 `build/agent-bundle/`。Linux 的实际构建目录位于 WSL 用户的
`.cache/testtools-agent-matrix/`；Windows 端只接收最终 Bundle 文件。可单独复核清单：

```powershell
.\scripts\validate-agent-bundle.ps1 -BundleDirectory .\build\agent-bundle
```

Bundle 的 `manifest.json` 使用 schema v2 的目标包 + 文件列表模型，记录 Agent/协议版本、平台、架构、ABI
或 libc 基线、包 root、安装相对路径、角色、大小、SHA-256、构建配置、固定构建标识和构建时间。
构建 wrapper 会检查 Linux ELF 架构、动态 glibc
依赖和最高 GLIBC 符号版本不得高于 `2.31`；交叉构建不代替板端契约测试。

GUI 通过统一 ADB 部署入口按设备用户空间分流：Android 读取 ABI，Linux 读取 `uname -m`、glibc
和 root adbd 状态。两端均推送所选包的全部文件并逐文件校验 SHA-256；Linux 部署只接受
`aarch64/glibc-2.31` 包，并使用
`/opt/testtools-agent/versions/<version>-<build-id>/`、`current`、`previous`、
`/var/log/testtools-agent/` 和 `/run/testtools-agent/` 管理版本、启动和回退；控制端口仍通过
ADB `forward` 接入。暂不接入 SSH、systemd 或 GUI 外的自动服务安装。

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
- `--agent-deploy-smoke <adb-serial>`：执行真实 ABI 识别、root 身份校验、部署、启动、控制转发、
  `system.info` 和 `serial.list`；要求 Agent 的有效 UID 为 0，不发送串口数据。
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
