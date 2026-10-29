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
├── application/   按 agent/device/serial/support/testing 组织的用例和生命周期
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
固定的 `third_party/platform-tools/` 也会复制到程序旁，包含 ADB、配套 DLL、版本与许可声明；
构建不下载依赖。ADB 来源和升级规则见 [`THIRD_PARTY.md`](THIRD_PARTY.md)。
固定 `third_party/scrcpy/` 同样随构建复制到输出目录；包含客户端、匹配 server、运行时 DLL 和许可证，
不包含第二份 ADB。投屏调用已解析的项目 ADB；`SCRCPY_PATH` 可显式指定客户端文件或完整工具目录。

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

修改 `src/agent/`、Agent 协议行为或板端返回字段时，必须在构建或实机同步前先递增
`src/core/protocol/agent_version.h` 中的 `AgentVersion`。这是上位机判断板端 Agent 是否需要
升级的唯一版本来源；以后新增或修改随包部署的 Runner/配置时也必须递增该版本，不再增加第二套
二进制指纹。同一版本号下重建不会触发升级，因此已用于任何板卡验证的版本号不得复用。
启动后的后台发现只做只读握手；用户点击
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

Linux 部署优先通过 `ldd --version` 判断 glibc；精简 Buildroot 没有 `ldd` 时，会只读执行
`/lib/libc.so.6` 或 `/lib64/libc.so.6` 获取版本。两条路径都必须识别为 glibc 且版本不低于
Bundle 声明的 2.31，否则拒绝部署。

### 4.3 板级能力与 RK Runner

`memory.capacity`、`storage.device.info` 和 `storage.device.health` 编译进通用 Agent，只读访问
`/proc`、`/sys`，不要求 RK SDK Runner 或能力标记。新增/修改 Agent 能力后必须递增 Agent 程序版本
并重建固定三目标矩阵；只修改 Board Profile 不改变板端程序时不需要重编 Agent。

RK PCBA 的 `echo_*` 程序仍由对应 SDK/rootfs 所有，不复制进通用 Agent Bundle，也不启动或抢占
`echo_pcbatest_server`。当前原厂 Runner 集成只保留为历史试点和未来 `rk_reference` 适配输入；不得
把旧标记作为三个原生只读能力的可用性条件。未来接入 Runner Pack、SDK/BSP 或 RK Reference 前，
先按 [`ADR-0003`](decisions/0003-stable-board-capabilities-and-providers.md) 固定 entry、版本、ABI、
摘要、参数硬上限和恢复规则，再进行单独审核。

所有构建目录和自产二进制均不提交 Git；`third_party/platform-tools/` 是随包分发的固定上游依赖，
其版本、摘要与许可声明必须一同保留。

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
  编解码、发送计划、异步队列、串口会话代次与关闭、工作区状态、资源冲突、板级任务响应校验和
  Board Profile 评估、共享 JSON 限额与后台任务所有权。
- Agent 主机契约测试：由 CTest 验证协议版本、严格字段拒绝、静态 Provider 分派和注入的 `/proc`、
  `/sys` 只读观察夹具，不访问真实设备。
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
- `--ui-snapshot <png>`：保存启动页快照；用 `--ui-page serial|terminal|device|files|mirror` 选择页面，
  用 `--ui-size 1366x768` 指定逻辑窗口尺寸。普通串口页再加 `--ui-serial-mode single|dual`；其他页面不混用模式参数，避免目标页被覆盖。
- UI 快照：检查主要分辨率和 DPI 下的裁切、重叠、文字清晰度、状态层级和主动作。
- 实机验证：串口连接、收发、双向验证和异常断开。
- Agent 阶段：协议单元测试、真实板端部署、能力枚举、稳定保持，再进行已知安全指令的收发测试。

构建通过不等于实机通过。当前验证摘要更新到 `STATUS.md`；只有会影响后续判断的里程碑和决定性
证据才写入 `DEVLOG.md`，避免把每次命令输出变成永久文档。

### 5.1 无设备运行与界面验收

```powershell
.\build\windows\x64\Release\TestTools.exe --offline --ui-page serial `
  --ui-size 1366x768 --ui-snapshot build/ui-serial-1366.png
cmake -S tests -B build/tests/host
cmake --build build/tests/host --config Release --parallel
ctest --test-dir build/tests/host -C Release --output-on-failure
```

主机 CTest 包含 `core-application-contract`、`file-transfer-contract` 和 `serial-recording-contract`；Agent 主机契约由 `src/agent/` 的独立 CMake 工程验证。
录制检查覆盖精确字节、UTF-8、成功 TX、停止/断线/退出、目标覆盖保护、注入的慢写与文件失败，以及有界显示投递；不访问串口设备。
原生 `serial-recording-ui` 使用离线串口和文件夹具检查录制对话框、工具栏/全局停止、显示截断、搜索/高亮/ANSI、偏好和窗口退出。
按需设置 `TESTTOOLS_RECORDING_SNAPSHOTS=1` 留下代表状态及实际尺寸/DPR 元数据，常规交互回归不必重新留图。
主机测试和离线快照均不要求开发板。`--offline` 跳过 ADB 发现、连接和设备退出清理；
与设备冒烟或 `--self-test` 同时使用时返回 2，不执行设备命令。

- `--ui-snapshot <png> --snapshot-delay 3500` 保存实际 Qt 窗口，并写入 `<png>.json` 记录诊断控件状态；
  延迟允许 100–30000 ms。ADB 终端按钮记录为 `adbTerminalButton`。
- `TESTTOOLS_DATA_DIR` 可指定隔离的本地数据目录，避免快照修改日常串口偏好或快捷指令。
- ADB 终端的验收范围见 [`features/ADB_TERMINAL.md`](features/ADB_TERMINAL.md)；
  启动冒烟使用独立假设备程序核对参数与回退，不以此代替真实交互式 Shell 验证。
- UI、构建和主机检查不能代替真实串口收发、热插拔或压力验证；只在对应能力变化时安排相关验证。
- 文件传输先用无 Qt 假后端验证固定设备/目录、单批互斥、逐项失败和取消，再用隔离的原生假 ADB 验证参数引用、
  二进制字节、目录、暂存替换、超时与输出限额。Qt 检查路径/选择门禁、设备切换旧回调、失败重试和传输/投屏同时退出。
  假 ADB 在主机 Shell 通过不证明 Android/Linux 工具兼容、SELinux、物理拔线或实际文件权限通过，需另列实机结果。
- 串口终端解析与输入契约见 [串口终端](features/SERIAL_TERMINAL.md)，主机检查覆盖跨批次 UTF-8/ANSI、
  行覆盖与擦除、纯光标序列刷新、限额、键码和终端偏好往返。Qt 假串口验证焦点、视图和字节映射，
  实机单独验证 U-Boot/Linux 的历史、补全和中断；这些交互状态由板端拥有。

### 5.2 按改动选择快照与回归

每批按受影响页面、共享组件和关键状态选择检查范围。窗口尺寸、缩放支持和交互质量要求仍见
[`UI_DESIGN.md`](UI_DESIGN.md)，不默认展开全部页面、尺寸、DPR 和状态的所有组合；36 张不是每轮要求。

| 改动 | 默认检查范围 | 扩大范围的依据 |
| --- | --- | --- |
| 局部控件或单页面布局 | 受影响页面的窄窗口与相关状态；宽度分配变化再看宽窗口，拥挤或文字尺寸变化补高 DPI 样本 | 共享布局受影响、发现裁切，或现有参考样本不能覆盖该问题 |
| 顶栏、导航、按钮等共享样式 | 受影响页面各选一次常用尺寸/缩放，再对最拥挤页面补宽窗口或高 DPI | 问题跨页面出现，或字体、DPI 处理、基础布局机制发生广泛变化时再展开完整矩阵 |
| 业务逻辑变化，界面布局未变 | 优先跑对应核心/交互测试，沿用适用的界面参考；新增状态只补该状态的快照 | 新状态改变了控件布局、长度、可见性或可操作性 |
| 仅文档 | 静态、入口/链接及差异检查 | 无需构建或重新抓图 |

- 优先覆盖与改动相关的启用/禁用、进行中/失败、长文本、展开/收起等状态；在少量代表样本中组合这些条件，
  不把每个状态再乘全部分辨率和缩放。截图数量随检查目的确定，不以减少或达到固定张数作为验收条件。
- 日常选择的代表快照逐张目检。使用完整矩阵时，元数据检查可以覆盖全部组合，目检选择代表样本，
  在 `STATUS.md` 现有验证表中分别说明：页面/尺寸/实际 DPR 检查范围、实际目检样本及结论、交互测试结果。
  元数据有效不能写成所有图片均已通过视觉或交互验收。
- 界面切换、焦点、精确发送字节、任务停止与失败等行为由对应交互/核心检查验证。截图不能替代这些断言，
  也不为截图检查新增一套通用测试框架。
- 一组改动稳定后集中验证。通过后只因新的相关修改、失败或未解决问题补查受影响部分；未受影响的检查结果沿用，
  不因每次小修重复全矩阵。只有广泛变更或明确覆盖缺口需要时才建立新全量参考，发版本身不自动要求重拍所有组合。

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

- 新增用户可见行为使用下一个次版本，修复同一行为的后续问题使用修订版本；不能为桌面界面改动同步改 Agent 版本。
- 一次可独立交付的迭代连同对应契约、必要测试和 STATUS 形成一个源码提交；提交信息使用中文并带版本。不要将每个临时修正都标成发布版本。
- 已保存的历史基线保持原有提交，不为补齐早期小版本号拆写历史。需要补版本标签时，先核对目标提交内的 `VERSION.txt`。
- `vX.Y.Z` 标签只能指向已提交、版本一致且验收边界明确的源码；未验证范围仍需记录，不将标签当作完整实机通过证明。生成物保留在 `build/`，不随源码提交。
- 提交、打标签、推送和发布分别执行。用户要求检查提交方案不等于授权执行这些动作；交付状态只写入 STATUS，远端状态用只读查询核对。

## 9. 按任务分工的开发试行

多代理只用于能独立验收的子任务。主负责人掌握需求、架构、文件所有权和最终差异，子任务返回定位、
修改及验证证据；采用结论前核对关键原文或源码。优先分派只读接口盘点、已有契约下的局部实现、
确定性测试和机械整理；线程生命周期、协议身份、跨层设计及设备操作由主负责人集中审核。

一次委派写清目标、允许读写文件、输入契约、验收条件与返回格式。默认先用一个工作代理；只传必要
上下文，不重复整段会话或让多个代理全文重审同一仓库。并行修改按文件划分，公共接口先确定；有
依赖的任务顺序执行。局部任务返工一次仍偏离契约时由主负责人接手，避免反复扩大代理数量。

模型按当次可用列表选择。边界清楚的任务可用 Luna 等较低成本模型，中等推理为起点，涉及调用链和
边界核对时按需提高；不把 Max 设为所有子任务的默认值。小于交接与复核成本的工作直接完成。
既有构建和检查在对应改动后集中执行一次，发现新问题再补验证，不能为省额度省略必要验收。

是否保留这种分工，依据实际返工次数、主负责人复核量和交付质量判断；如有可靠的任务用量数据，再
比较总消耗。不用 API 单价推算订阅额度，也不承诺固定节省比例。多代理会增加独立上下文和工具工作，
更高推理通常也增加用量；这些边界见 [官方子代理说明](https://developers.openai.com/codex/subagents)。
先在本项目试行，不据单次结果安装全局规则或额外工具。

## 原生 UI 与文件传输回归

先构建 Release，再使用同版本 Qt 的 CMake 前缀配置 `tests/ui`（需要 Qt Test）：

```powershell
cmake -S tests/ui -B build/tests/ui -DCMAKE_PREFIX_PATH=<Qt安装目录>
cmake --build build/tests/ui --config Release --parallel 4
ctest --test-dir build/tests/ui -C Release --output-on-failure
```

这些测试链接当前产品的 Release 对象，覆盖真实 Widgets 交互、串口精确字节、Windows 原生图标、真实回环协议连接/失联与主机 ADB 异常夹具，默认不访问真实设备。
修改产品代码后必须先重建产品和此测试目标，不能以旧对象结果验收。测试证据默认写入该 CMake 构建目录的 `evidence/`，偏好写入 `fixture-data/`，避免覆盖旧迭代记录。可用 CMake 的 `TESTTOOLS_TEST_OUTPUT` 或手动运行时的同名环境变量指定其他证据目录。
`directory-perf` 为独立本地目录元数据性能样本，不纳入默认 CTest。

`device-smoke <adb.exe绝对路径> <设备序列号>` 会在明确选择的设备 `/data/local/tmp/testtools-file-qa-<数字>` 下创建、修改并清理自有夹具，
测试文件收发、目录链接与权限错误；需要明确的实机验收授权，不能把它当只读检查。
`mirror-host-probe <scrcpy.exe绝对路径> <adb.exe绝对路径> <设备序列号>` 仅作真实外部窗口接入试验，退出收敛自己创建的进程；
挂接成功不代表输入法、回嵌和 APK 拖放验收通过。这两个可执行文件均不由 CTest 自动运行。

截图诊断按目标页选择 `--offline --ui-page serial|terminal|device|files|mirror --ui-size 1366x768 --ui-snapshot <PNG>`；
只有普通串口页追加 `--ui-serial-mode single|dual`。检查侧车 JSON 中的实际可见页面，不能只根据输出文件名判断。
`QT_SCALE_FACTOR` 是进程缩放系数，不能直接当作实际 DPR；以 JSON 记录的逻辑尺寸和实际 DPR 为准，必要时调整系数补拍对应样本。
PNG 使用设备像素，逻辑窗口尺寸与物理屏幕分辨率分别记录。快照选择与复查范围遵循第 5.2 节。

`ui-smoke --live-files <设备序列号> <adb.exe绝对路径>` 为显式只读文件页实机检查：跳过 Agent 准备和设备自动发现，
验证进入自动打开根目录、浏览 `/sdcard`、返回文件页刷新与关闭；只读取设备目录，不执行 push/pull、安装或串口发送。

`iteration-smoke` 默认使用假串口，验证独立页面、占用交接、偏好迁移、万行增量显示和地址草稿。
`iteration-smoke --live <Windows串口> <ADB设备序列号>` 会真实打开指定串口，发送 Shell 检查、历史/补全/中断和 10500 行有上限的合成输出，连接并重连当前 Agent，并在关闭终端后将串口交给普通监视器；必须在已授权且确认是 Debug Shell 的端口上显式执行。该模式不由 CTest 自动运行，不代表物理拔线、重启启动日志或长时压力通过。
