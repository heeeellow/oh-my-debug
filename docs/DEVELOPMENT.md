# 开发手册

[返回项目首页](../README.md)

## 1. 环境与目录

桌面程序使用 Windows x64、C++17、Qt 6.11.2 `msvc2022_64` 和 Qt VS Tools。
本轮验证环境为 Visual Studio 2026、MSVC 14.44.35207；工程保留这一已验证编译器版本。
VS Installer 需安装对应 MSVC 组件。其他编译器组合需要单独验证，不能只重定向后直接视为兼容。
板端 Agent 不依赖 Qt，通过 CMake 面向 Android/Linux 构建。

| 路径 | 职责 |
| --- | --- |
| `src/core/` | 协议、值对象、串口基础逻辑 |
| `src/application/` | Agent、设备、串口和终端用例、后台生命周期 |
| `src/adapters/` | Windows COM、ADB、TCP、进程与文件等具体实现 |
| `src/ui/` | Qt 页面、组件、主题与资源 |
| `src/agent/` | 无 Qt 板端服务 |
| `src/diagnostics/` | 显式调用的产品诊断入口 |
| `tests/`、`tests/agent/` | 无设备核心契约与 Agent 主机测试 |
| `tests/ui/` | 使用假设备的原生界面检查 |
| `tests/manual/` | 不参与默认构建或 CTest 的实机/性能工具 |
| `build/` | 编译结果、录屏快照与本地证据，不提交 |

目录按已有职责组织，不为候选功能创建空目录。依赖只保留编译与许可所需内容；
上游版本、本地适配与校验清单见 [THIRD_PARTY.md](THIRD_PARTY.md)。

### 1.1 Visual Studio 视图

打开根目录 `OhMyDebug.slnx`。解决方案只有桌面程序一个可构建项目；
项目筛选器与磁盘路径一致，例如 `src/application/serial`、`src/ui/pages` 和 `third_party/libvterm`。
`docs` 仅提供 README、LICENSE、使用说明、开发手册和架构五个快捷入口。
`agent` 提供板端源码、CMakeLists、共享版本头、协议、主机测试和矩阵构建脚本的紧凑入口。
这些是源码导航文件，实际编译使用独立 CMake 与交叉工具链；F5 仍启动桌面程序。
其他测试和辅助脚本可从磁盘或“打开文件夹”浏览。

新增源码先加入 `OhMyDebug.vcxproj`，再运行：

```powershell
.\scripts\sync-vs-layout.ps1
.\scripts\sync-vs-layout.ps1 -Check
```

脚本按项目清单生成筛选器，固定排序和标识；重复运行不改写一致的文件。
支持 Windows PowerShell 5.1 和 PowerShell 7。若 VS 提示外部更改，选择重载。

## 2. 桌面构建

在 Qt VS Tools 中注册工程指定的 `6.11.2_msvc2022_64`，打开解决方案选择 `x64 / Release`。
日常体验使用 Release，Debug 用于调试。命令行可在 Developer PowerShell 中执行：

```powershell
$qtMsBuild = Join-Path $env:LOCALAPPDATA 'QtMsBuild'
msbuild .\OhMyDebug.vcxproj /m /p:VCToolsVersion=14.44.35207 /p:Configuration=Release /p:Platform=x64 `
  /p:QtMsBuild="$qtMsBuild"
```

未找到 MSBuild 时通过 Visual Studio Installer 自带的 `vswhere` 查询，不在脚本写入个人路径。
产物位于 `build/windows/x64/Release/OhMyDebug.exe`。
Qt 运行时、固定 ADB、scrcpy 和已有 Agent Bundle 会复制到程序旁。
工具箱的固定 Windows iperf3/运行库/许可同步复制到 `iperf3/`；构建前校验
`third_party/iperf3/`、`third_party/nlohmann/` 的摘要，失败时停止。
构建不联网升级依赖，也不执行设备操作；缺少 Bundle 时仅允许开发启动，不能打完整发布包。

工程固定 `VCToolsVersion=14.44.35207`，避免 Developer Shell 默认版本覆盖并混用 LTCG 对象。
命令行构建同时显式传入该全局属性；本机 VS 2026 的默认工具集会选择较新的编译器，不能仅凭已进入 Developer Shell 推断实际版本。
隔离构建可指定 `OutDir`、`IntDir` 与 `TestToolsAgentBundleDirectory`，不影响正在使用的程序。

`VERSION.txt` 是桌面版本唯一来源，构建时生成 `app_version.h`，供程序与 Windows 版本资源共用。
品牌和版权常量在 `src/app_info.h`。内部设置目录、协议标识、Agent 文件名和部署路径保持 TestTools 兼容。
生成文件不加入工程源码或 Git。清理废弃桌面对象可用 `scripts/clean-desktop-objects.ps1`，先检查其提示。

## 3. 验证

先执行与修改直接相关的检查，再根据失败或剩余风险扩大范围：

```powershell
.\scripts\check-baseline.ps1
cmake -S tests -B build/tests/host
cmake --build build/tests/host --config Release --parallel
ctest --test-dir build/tests/host -C Release --output-on-failure
cmake -S src/agent -B build/agent/host-tests -DTESTTOOLS_BUILD_TESTS=ON
cmake --build build/agent/host-tests --config Release --parallel
ctest --test-dir build/agent/host-tests -C Release --output-on-failure
```

若同机安装多个 MSVC 版本，原生 UI 检查应使用与产品对象一致的编译器。
可进入选定的 Developer PowerShell 后使用 Ninja，显式指定 `-G Ninja`、`-DCMAKE_BUILD_TYPE=Release`
及可用的 Ninja 路径；不要把别的编译器生成的 LTCG 对象混入同一测试。

原生 UI 检查链接已构建的 Release 产品对象，默认 CTest 仅使用假设备：

```powershell
cmake -S tests/ui -B build/tests/ui -DCMAKE_PREFIX_PATH='<Qt SDK path>'
cmake --build build/tests/ui --config Release --parallel
ctest --test-dir build/tests/ui -C Release --output-on-failure
```

隔离产品对象时增加 `-DTESTTOOLS_HOST_OBJECT_DIRECTORY='<absolute Release object directory>'`，
同时保持测试与产品的编译器版本一致。`toolbox-contract` 覆盖协议、租约、未知结束与测速协调；
`toolbox-ui` 覆盖两页的门禁、确认取消、结果与关闭，不操作真实引脚或发送实际测试流量。

`serial-workspace-smoke` 验证页面分离、偏好和端口交接；`terminal-throughput-smoke`
包含连续输出、空闲交互与回显延迟检查。计时检查应在低负载下单独运行，不能把并行编译造成的延迟当成产品结论。
`tests/manual/` 中的工具必须显式选择目标构建；`terminal-live-smoke` 会打开并发送到真实串口，
`device-smoke` 会访问真实设备，它们不在默认检查中。任何发送、写入、重启或压力操作均需明确设备与授权范围。

`toolbox-smoke` 默认仍为无设备夹具。显式 `--live-network <serial> <forward-port> <runtime-directory> <output-directory>`
会通过真实服务发起双向 TCP/UDP 流量；当前验收入口固定核对 192.168.1.211 ↔ 192.168.1.111，不适用于其他网络配置。
`--live-inspect` 使用相同参数只读取元数据和抓图；`--chart-report <report.json> <output.png>` 从已有样本重绘，不访问设备。
`--live-board-inspect` 使用相同四个参数，在显式选择的真实服务上发现设备控制接口、读取首个端点并抓图，断言任务为 `gpio.board.read`，不发送 GPIO 写入。
`maintenance-backend-smoke --inspect-loader <adb-program> <serial> <output.txt>` 仅执行当前设备的 Loader 支持信息读取，不发送重启请求；默认 CTest 不访问真实设备。
Linux 主机的 toolbox-contract 另含两类驱动接口的隔离文件系统夹具，模拟内核 show/store；Windows 主机仅运行可移植契约。

无设备界面快照：

```powershell
.\build\windows\x64\Release\OhMyDebug.exe --offline `
  --ui-page terminal --ui-size 1440x900 --ui-snapshot '<absolute png path>'
```

`--offline` 不启动 ADB 或真实串口；不可与实机诊断入口混用。
工具箱页面参数为 `--ui-page gpio` 或 `network`。多次快照顺序启动并等待退出，
使用独立 `TESTTOOLS_DATA_DIR` 和 `TESTTOOLS_TOOLBOX_PREFERENCES_DIRECTORY`，避免碰到用户偏好。
界面改动至少检查目标分辨率、控件状态与异常路径。
构建、静态、模拟、实机、异常和压力结果分别记录，未执行写“未验证”。
内置 `--self-test` 可能访问设备，不能当作离线检查调用。

## 4. Agent 构建

板端服务与桌面程序在同一仓库开发，分别生成不同平台的程序。

| 修改范围 | 开发与构建流程 |
| --- | --- |
| 只改桌面界面或主机用例 | 在 VS 构建桌面项目，校验并复制已有 Bundle；不启动 NDK/WSL |
| 修改 Agent 实现 | 先运行第 3 节的 Agent 主机契约；再交叉构建三个目标，最后重建桌面以携带新 Bundle |
| 准备完整发布 | 按 Agent 矩阵 → 桌面 Release → 发布包的顺序执行 |

`src/agent/` 保存板端实现，`src/core/protocol/` 保存共享协议与版本，`tests/agent/` 保存主机检查。
Windows 上的 CMake 主机测试产物用于验证协议与逻辑；开发板使用 NDK/Linux 交叉工具链生成的目标产物。

Agent 生产行为、协议字段或随包部署的 Runner/配置变化时，先递增
`src/core/protocol/agent_version.h` 中的 `AgentVersion`。纯文件移动、命名统一不改变协议语义。
不得重复使用已在设备验证的版本号承载新的行为。

```powershell
.\scripts\build-agent-matrix.ps1 -NdkRoot '<Android NDK path>' `
  -CMakeProgram '<cmake path>' -NinjaProgram '<ninja path>'
.\scripts\validate-agent-bundle.ps1 -BundleDirectory .\build\agent-bundle
msbuild .\OhMyDebug.vcxproj /m /p:VCToolsVersion=14.44.35207 /p:Configuration=Release /p:Platform=x64 `
  /p:QtMsBuild="$env:LOCALAPPDATA\QtMsBuild"
```

固定矩阵为 Android `armeabi-v7a`、`arm64-v8a`（最低 API 23、Bionic、静态 C++ 运行库）
及 Linux `aarch64/glibc-2.31`。Android 使用 NDK，Linux 在 WSL2 Ubuntu-24.04 的用户目录编译；
可信工具链通过 `scripts/install-linux-aarch64-toolchain.sh` 安装，不需要 sudo。
可通过 `-WslDistribution`、`-LinuxToolchainRoot` 指定环境。CMake/Ninja 来自参数或 PATH。

最终 Bundle 位于 `build/agent-bundle/`，schema v2 记录目标、文件、大小和 SHA-256；
校验覆盖三个目标的 Agent/iperf3 Runner 及当前 AgentVersion。板端 iperf3 从固定原始源码包
通过 `third_party/iperf3/CMakeLists.txt` 构建，配置探测在输出目录进行，不改写上游压缩包。
桌面构建只复制，不隐式启动交叉编译。
存在且校验通过的 Bundle 会进入 `build/windows/x64/<Configuration>/agent-bundle/`，发布脚本再将其纳入 ZIP。
缺少 Bundle 时桌面构建给出警告，陈旧或损坏的 Bundle 阻止复制；发布打包要求完整 Bundle。
Android 与 Linux 不能共用二进制，内核版本也不能代替 ABI、libc 和设备权限验收。
部署与协议规则见 [架构说明](ARCHITECTURE.md) 和 [协议契约](AGENT_PROTOCOL_V1.md)。
`TESTTOOLS_BUILD_FAKE_AGENT=ON` 仅构建主机协议夹具，不是产品启动项。

## 5. 打包与公开前检查

### 交付形式

当前脚本生成 **ZIP 便携包**，用户解压完整目录后运行 `OhMyDebug.exe`。单文件 EXE 尚未实现。

| 形式 | 用户操作 | 对本项目的影响 |
| --- | --- | --- |
| ZIP 便携包 | 完整解压一次，后续直接运行；搬移时复制整个目录 | 沿用已有运行目录与校验流程，便于保留多个版本、检查依赖和定位问题 |
| 单文件便携 EXE | 双击一个文件，启动器自动准备运行文件 | 拟采用首次解包到用户缓存目录、同版本复用；需要处理缓存校验、并发启动、子进程占用、版本隔离和清理。启动耗时与包体积均需实测 |
| EXE 安装包 | 下载一个文件，安装后从快捷方式启动 | 安装完整运行目录，可增加快捷方式和卸载；安装器和升级/卸载行为需单独实现与验收 |

用户提出的 SSCOM 式体验指单文件、免安装、双击启动；本页不推断 SSCOM 内部使用何种链接或打包技术。
本项目当前使用动态 Qt 库、插件、ADB、scrcpy、iperf3 和多目标 Agent，单文件便携方案仍需将这些运行文件释放到磁盘。
设置沿用现有用户目录，打包成一个 EXE 不会自动把配置、日志和缓存也变成一个文件。

建议先保留 ZIP 作为可检查的基础交付；若选单文件便携 EXE，使用同一份已验证运行文件清单构建启动器，
保留功能与依赖版本一致。两种容器都可以压缩，不能仅根据扩展名承诺体积更小或启动更快。
格式选择不改变设备操作的确认与任务规则；形式未确认前不生成新包、不新增安装器或自解包代码。

依据：[Qt 6.11 Windows 部署](https://doc.qt.io/qt-6.11/windows-deployment.html)说明依赖目录的收集与安装包交付；
[Inno Setup 官方说明](https://jrsoftware.org/ishelp/topic_whatisinnosetup.htm)说明单 EXE 安装包能力，作为安装器候选参考，不代表已选用。

### ZIP 打包流程

先提交并核对目标源码版本，确认完整 Agent Bundle，构建对应桌面 Release，再提供同版本 Qt 源码目录，以收集许可与第三方声明。
板端代码或部署内容变化时按上一节执行 Agent 矩阵；仅桌面/文档变化可复用已经校验、版本一致的 Bundle。
发布内容应同时携带[本版更新说明](releases/0.18.1.md)，发布脚本当前仅自动收集使用手册和第三方说明，补齐更新说明的收集随最终打包方式一起实施。

现有 ZIP 脚本入口：

```powershell
.\scripts\package-release.ps1 -QtSourceDirectory '<Qt 6.11.2 source path>'
```

脚本要求干净的 Git 工作树，核对产品版本、Release 标记、Qt 版本、随包工具与 Agent 摘要，
按清单收集运行文件、使用说明、许可与版本清单，生成 ZIP 和 SHA-256。
不复制 PDB、测试程序、个人设置或整个开发输出目录，不覆盖已有包。
`-RuntimeDirectory` 可指定隔离构建；`-OutputDirectory` 可指定新目录。
尚未提交时用 `-AllowDirty` 生成明确带 `preview` 的本地预览，不能将它作为正式发布记录。

解压到新目录后，在移除 Qt 环境变量和 SDK PATH 的进程中离线启动，检查版本与目标分辨率快照。
本机启动不等于未安装运行库的干净 Windows 已验证；后者需在发布前单独执行。

公开前还需复核历史提交中的个人信息、第三方对应源码的提供方式和分发义务，
包括所带 Cygwin 运行时的准确对应源码/补丁；现有许可文本和源码入口不能替代这一核对。
并进行真实设备回归。公开源码、发布二进制、远程改名、推送与打标签均是独立操作。
仓库中的 LICENSE 不代表已完成名称权利检索。

## 6. 维护规则

Qt 只进入 UI/GUI 桥接；无 Qt 层通过接口协作，UI 不包含具体适配器。
阻塞 I/O 放后台，回调校验代次，关闭时取消并等待所属任务；队列和等待均有边界。
通用串口不按设备名称隐式改行为，普通计划发送和压测保持独立。
新增能力先定义固定 ID、参数、资源与风险，不提供任意 shell RPC。

行为写入对应 `docs/features/` 契约，当前进度只汇总到 `STATUS.md`；
长期设计取舍放 `docs/decisions/`，历史证据放 `DEVLOG.md`，不为每次修改增加新文档。
提交前运行 `scripts/check-project-docs.ps1` 和 `scripts/check-baseline.ps1`，复核实际 Git 差异。
保留无关用户修改，不自动暂存、改写历史或推送；提交说明使用中文。

## 7. GitHub SSH 连接

网络限制 22 端口时，GitHub 提供 `ssh.github.com:443`。在用户 SSH 配置中为 GitHub 单独设置，
其他服务器仍使用自己的 Host 配置：

```sshconfig
Host github.com
    HostName ssh.github.com
    Port 443
    User git
```

配置方法见 [GitHub 官方说明](https://docs.github.com/en/authentication/troubleshooting-ssh/using-ssh-over-the-https-port)。
首次连接时核对官方公布的主机指纹，保留 SSH 的主机校验。

在仓库目录依次检查：

```powershell
ssh -G git@github.com
ssh -T git@github.com
git ls-remote origin
git push --dry-run origin master v0.16.0
```

`ssh -G` 用于查看实际生效的主机与端口。`Permission denied (publickey)` 表示已进入认证阶段，
需要检查公钥登记、所用 SSH 客户端和密钥加载情况。GitHub 显示认证成功但不提供 Shell 时，
`ssh -T` 仍可能返回退出码 1，判断时应结合输出。

Windows 可能同时安装系统 OpenSSH 和 Git 自带的 SSH。若系统客户端可用而 Git 调用失败，
可在当前仓库指定系统客户端：

```powershell
$gitSsh = (Join-Path $env:WINDIR 'System32/OpenSSH/ssh.exe').Replace('\', '/')
git config --local core.sshCommand "$gitSsh -o ConnectTimeout=15"
```

这项配置保存在本地 `.git/config`，不随源码提交。修改后重新执行远端读取和推送预检查；
`--dry-run` 不实际更新远端分支或标签。
