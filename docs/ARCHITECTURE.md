# 架构说明

[返回项目首页](../README.md)

本页说明已经存在的分层、数据流和所有权。当前完成度与验证结果只写在
[STATUS.md](STATUS.md)，候选方向保留在 `decisions/`，不为方案中的名称预建类或目录。

## 1. 分层与源码

| 层 | 目录与代表对象 | 边界 |
| --- | --- | --- |
| 界面 | `src/ui/`：MainWindow、页面、组件 | Qt 展示、GUI 线程桥接、焦点与输入；不拥有业务协议 |
| 用例 | `src/application/`：SerialSession、AgentConnectionService、FileTransferService | 会话、取消、超时、连接代次与结果；无 Qt |
| 基础逻辑 | `src/core/`：协议、串口值对象、编解码与接口 | 平台无关数据和可独立验证的规则；无 Qt |
| 具体适配 | `src/adapters/`：Windows COM、ADB、TCP、进程、文件 | 实现 core 接口，装配 application 服务；无 Qt |
| 板端 | `src/agent/`：AgentService、串口桥、固定板级能力 | 标准 C++17 和平台 API，不依赖 Qt |
| 诊断与测试 | `src/diagnostics/`、`tests/` | 显式诊断入口与独立夹具；测试不是产品启动项 |

界面调用 application 用例，用例依赖 core 接口，适配器实现接口。
默认服务工厂负责组合具体实现，UI 不包含具体适配器头文件。
`src/main.cpp` 负责 Qt 进程启动和显式诊断参数，品牌常量在 `src/app_info.h`，
桌面版本来自 `VERSION.txt`。第一方 C++ 命名空间统一为 `testtools`。
对外名称调整不改变既有 Agent 协议、部署目录或个人设置位置。

Visual Studio 筛选器遵循实际目录，桌面解决方案只含一个可构建工程。
解决方案另保留紧凑的 `agent` 源码导航；Agent 与测试继续独立 CMake 构建，
见 [开发手册](DEVELOPMENT.md#11-visual-studio-视图)。

## 2. 设备与服务

ADB 只作为发现、部署和回环转发通道。串口托管、硬件信息与板级测试通过固定 Agent 协议执行，
不以 ADB `forward dev:/dev/...` 或任意 Shell RPC 承载业务。
桌面的文件、维护与投屏适配器可执行各自固定且校验过的 ADB 命令，边界由功能契约定义。

`AgentConnectionService` 拥有版本判断、按需部署、握手、能力发现及失败时的转发清理。
主窗口在后台执行此用例，并核对当前设备和连接代次再更新控件。
ADB 在线、选中设备和 Agent 就绪是不同状态，本地 Windows 串口独立可用。
健康探测使用独立短超时连接，不能占用显式连接操作或在 GUI 线程等待。

桌面 `AgentClient`、`AgentSerialTransport` 和板端 `agent_main.cpp` 使用 standalone Asio
完成 TCP 连接、监听与读写。控制请求和串口数据客户端经 ADB 回环转发连接设备；
Windows 本地串口由 CSerialPort 适配器负责。Asio 不依赖 Qt，供主机与板端共同使用。

`AdbTerminalService` 固定用户选中的在线设备，进程适配器启动独立系统终端。
Shell 生命周期不进入 Agent RPC，不与文件页共享当前目录；内嵌 Shell 仍属候选。

## 3. 串口与终端

```text
PC COM / Agent 数据通道
          ↓
    SerialSession ───────→ SerialRecorder（独立线程、文件与队列）
          ↓
    TerminalEngine（单工作线程、libvterm、历史与快照）
          ↓ 不可变 TerminalFrame
    SerialTerminalWorkspace（GUI 通知、会话状态、输入桥接）
          ↓
    TerminalGridWidget（可见行、字形缓存、选区、搜索、输入法）

键盘 / 粘贴 → TerminalEngine 编码 → 当前代次发送队列 → 传输
```

`SerialSession` 拥有异步收发和普通计划发送，`SerialLinkVerifier` 单独管理双向验证及可靠性压测。
普通收发与终端通过 `SerialEndpointWorkspace` 复用端点连接、占用登记、录制和异步关闭，
各自拥有会话、偏好与显示缓冲，不能共享活动 RX 状态。通用行为不按设备名称隐式变化。

普通 `SerialWorkspace` 使用文本收发视图和快捷指令侧栏，`AtCommandDialog` 负责指令编辑。
`AtCommandStore` 只存指令库；`SerialPreferencesStore` 单独存通道和工作区偏好。
`SerialPreferencesWriter` 串行写入一个文件，只保留最新待写快照；UI 合并短时变更并显示保存错误。
PC 端口枚举有独立后台任务，选择由 `chooseLocalPort` 处理，手工输入来源单独记录。

终端页只装配一个端点，后台按连续字节流解析，不在普通日志上按时间推断帧边界。
字符网格按实际 DPR 缓存可见行和有限字形，选区、搜索和光标单独叠加；空闲不轮询解析或渲染。
按键、粘贴和固定 VT 查询应答通过当前代次的发送队列，缩放不隐式发送 Shell 命令。
`serial_terminal.*` 只保留行尾、键值与粘贴辅助，不再持有旧解析器。

录制位于显示裁剪之前。显示队列满时报告缺口；录制队列满则停止录制并报告不完整。
录制线程独占文件，关闭时排空已接受数据。窗口退出先提交最后偏好，再等待所属扫描、录制和写入收尾。
具体限额、时间戳、高亮、粘贴与兼容范围见 [串口契约](features/SERIAL.md)
和 [终端契约](features/SERIAL_TERMINAL.md)。

## 4. 文件、维护、投屏与信息

| 用例 | 所有权与执行方式 | 契约 |
| --- | --- | --- |
| 文件传输 | FileTransferService 固定设备、路径、任务 ID 与取消；适配器执行固定命令及执行前检查 | [文件](features/FILE_TRANSFER.md) |
| 设备维护 | DeviceMaintenanceService 拥有动作、阶段、超时与部分日志文件；适配器只允许固定操作 | [维护](features/DEVICE_MAINTENANCE.md) |
| 投屏 | ScreenMirrorService 固定会话设备，scrcpy 后端只关闭自己拥有的子进程，私有环境指定统一 ADB | [投屏](features/SCREEN_MIRROR.md) |
| 硬件信息 | BoardInfoService 顺序执行三个固定只读项，独立连接、取消和代次，不进行规格比较 | [信息](features/BOARD_INFO.md) |

目录读取和传输分别持有设备快照与取消状态，旧目录回调不能覆盖新设备。
`FileListModel` 只保存显示数据，排序和绘制不访问文件系统；`FilePane` 管理列表、选择和位置。
Windows Shell 图标后台读取并有界缓存，QPixmap 与模型通知只在 GUI 线程产生。
目录收藏是按设备限额保存的 UI 偏好，不缓存真实设备目录内容。

`MainWindow` 汇总已有任务快照建立同设备维护门禁，各入口仍自行检查。
底栏只显示快照并提交带任务 ID 的停止意图，不新增通用任务框架。
写入、Root/Remount、重启和压力测试与只读信息明确区分；确认界面不能代替执行前检查。

## 5. 板端、协议与部署

`ToolboxWorkspace` 装配 GPIO 与网络两个页面，`ToolboxService` 在无 Qt 层持有固定设备的请求、
独立控制连接、停止/复核及报告。`core/toolbox/` 保存参数白名单、主机接口与有界 iperf JSON 解析，
`WindowsToolboxHost` 提供网卡、路由和受管客户端进程。阻塞执行与续租均由后台工作线程驱动，
UI 只读取快照并投递停止意图。偏好和报告写入也在后台，Qt 控件只在 GUI 线程访问。

板端 `ToolboxAgent` 管理实例/所有者/操作票据、进度和租约，复用现有单板级任务管理器；
`toolbox_platform.cpp` 独占 GPIO 句柄和固定随包 Runner 的子进程。启动响应丢失先废弃/复核原操作，
不重复执行。资源结束未知由用例保持门禁；固定动作、限额和结果语义见 [工具箱契约](features/TOOLBOX.md)。
`BoardGpioRegistry` 在 Agent 内适配固定的公司 sysfs ABI，按驱动、设备树和原生行信息关联端点；
端点标识不接受路径透传，执行时重新核对身份。它复用 ToolboxAgent 的任务归属，保持原生 GPIO 申请规则。

Agent 默认只监听板端回环地址，桌面经当前设备的 ADB 动态转发连接。
控制连接使用长度前缀帧和有输入限额的 UTF-8 JSON；串口使用独立数据连接，避免持续数据阻塞控制。
字段、能力、会话与错误定义以 [AGENT_PROTOCOL_V1.md](AGENT_PROTOCOL_V1.md) 为准，不在本页复制协议。

原有只读板级能力通过静态 ProviderBinding 关联固定能力 ID、风险、资源与执行函数；
工具能力由上述 ToolboxAgent 的固定白名单校验参数并装配执行上下文。
只使用既定的 `agent_native`、`runner_pack`、`sdk_bsp` 和可选 `rk_reference` 来源，
不建立动态插件或任意路径执行。资源冲突由板端裁决，桌面预检查用于提前提示。
读取结果保留 observation，与能力执行成功和规格判断区分。设计取舍见
[ADR-0003](decisions/0003-stable-board-capabilities-and-providers.md)。

Bundle v2 按用户空间选择 Android ARMv7/ARM64 或 Linux aarch64/glibc-2.31，
每目标包含 Agent 与 iperf3 Runner，逐文件校验大小和 SHA-256。Runner 从 Agent 所在目录固定定位，
不接受外部路径。AgentVersion 是需要升级的唯一版本标识，桌面版本独立。
内核版本不是二进制目录维度，更换内核后仍需重验驱动、节点、termios、权限和安全策略。

Android 使用受控临时目录部署；Linux 使用 `/opt/testtools-agent/versions/` 的版本目录及
`current/previous` 回退、独立运行和日志目录。失败不能破坏共享 ADB server。
当前信任边界依赖已有 ADB 授权和回环监听；直接网络暴露、认证、包签名和量产服务集成需另行设计。

## 6. 线程与修改规则

- GUI 只处理界面和轻量状态映射，阻塞 ADB、串口、网络和文件操作放后台。
- 每个线程、进程、连接和文件有明确所有者，关闭先拒绝新请求，再取消、等待并释放回调。
- 回调核对会话或连接代次；不得捕获缺少生命周期保护的 Qt 控件。
- 队列、输入、等待与历史均有边界；不能以无限缓存代替可靠性。
- 新功能先补对应契约和无 Qt 的状态逻辑，再装配界面，不为规划中的职责创建空类型。
