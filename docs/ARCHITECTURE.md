# TestTools 总体架构

[返回项目首页](../README.md)

## 1. 架构目标

TestTools 从串口助手演进为设备中心的调试与测试工作台。架构需要同时保证：

- Qt 限于 Windows 表现层和 GUI 线程桥接，核心逻辑可在没有 Qt 的环境复用和测试。
- ADB 是可替换的发现、部署和隧道通道；用户手动文件传输、投屏与诊断维护由固定主机适配器提供，不承载串口或板级测试业务语义。
- 用户主动打开的外部 ADB 终端只交给系统终端交互，不进入 Agent RPC、串口数据通道或自动测试执行路径。
- 板端操作由无 Qt Agent 统一提供，不以 Windows 端拼接任意 shell 命令实现测试项。
- 简单测试可以使用通用运行页，串口、音频、图像等复杂能力可以绑定专用界面。
- 长任务最终支持进度、取消、资源互斥、断线恢复、板端重启恢复和产物下载。
- 演进过程中保持已落地串口能力可用，并通过明确兼容边界逐步替换临时实现。

产品范围见 [`PRODUCT.md`](PRODUCT.md)，当前实现与架构债务见 [`STATUS.md`](STATUS.md)。

## 2. 依赖方向

```text
┌──────────────────────────────────────────────────────────────┐
│ Qt Presentation                                             │
│ MainWindow / Pages / Dialogs / Qt thread dispatch           │
└──────────────────────────────┬───────────────────────────────┘
                               │ user intent + view state
┌──────────────────────────────▼───────────────────────────────┐
│ Application                                                  │
│ Device session / serial use cases / jobs / history          │
└──────────────────────────────┬───────────────────────────────┘
                               │ interfaces + value objects
┌──────────────────────────────▼───────────────────────────────┐
│ Core                                                         │
│ Protocol / codec / serial model / queues / test engine       │
└──────────────┬───────────────────────────┬───────────────────┘
               │                           │
┌──────────────▼──────────────┐ ┌──────────▼──────────────────┐
│ Host transports/adapters   │ │ Local device adapters       │
│ ADB / Agent client         │ │ Windows COM / process       │
└──────────────┬──────────────┘ └─────────────────────────────┘
               │ loopback tunnel
┌──────────────▼───────────────────────────────────────────────┐
│ Board Agent, standard C++17 + platform APIs                 │
│ session / capability / resource / job / artifact managers  │
├──────────────────────────────────────────────────────────────┤
│ serial and future DDR/storage/audio/network/GPIO runners    │
├──────────────────────────────────────────────────────────────┤
│ ARM Android / ARM Linux platform adapters                   │
└──────────────────────────────────────────────────────────────┘
```

依赖只能向下。`src/core/`、`src/application/`、`src/adapters/` 和 `src/agent/` 不得出现 `QString`、
`QByteArray`、`QObject`、Qt 容器或 Qt 线程类型。UI 可以依赖 core/application 接口，反向依赖不允许。

## 3. 源码所有权映射

架构图包含稳定职责和演进目标，不能把目标类名写成当前事实。源码所有权按以下路径划分；具体完成度
和开放债务只在 [`STATUS.md`](STATUS.md) 维护：

| 层 | 源码所有权 | 边界 |
| --- | --- | --- |
| Presentation | `src/ui/main_window.*`、`src/ui/pages/serial_workspace.*`、`src/ui/theme.*` | Qt 控件、文案和 GUI 线程映射 |
| Application | `src/application/{agent,device,serial,support}/` | 设备连接、串口会话、日志缓冲与偏好持久化；Qt 只读取快照 |
| Core | `src/core/{device,protocol,serial,support}/` | 协议、值对象、接口、编解码、发送与测试逻辑；标准 C++，不得依赖 Qt |
| Host adapters | `src/adapters/{adb,agent,device,serial}/` | ADB、进程、Agent TCP 和 Windows COM 的具体实现；不保存产品用例状态 |
| Board Agent | `src/agent/agent_service.*`、`serial_bridge.*`、`board_capability.*`、`board_test_job.*`、`native_board_observations.*` | 固定能力、参数校验和板端资源访问 |
| 快捷指令与偏好 | `src/application/serial/at_command_library.*` | 无 Qt 的快捷指令（AT/HEX）与串口界面偏好值对象、边界校验和版本化本地 JSON；界面只负责编辑、选择与发送意图 |
| 串口终端 | `src/application/serial/serial_terminal.*` | 无 Qt 的 UTF-8/ANSI 解析、有界显示单元、键码/粘贴编码；复用串口会话，不产生隐式终端应答，命令历史由板端拥有 |
| 串口页面占用 | `src/application/serial/serial_port_claims.*` | 无 Qt 的主机资源登记；Windows 端口别名归一化、按设备保持 Agent 单会话；登记不替代 OS/Agent 裁决 |
| Android 投屏 | `src/core/device/screen_mirror.h`、`src/application/device/screen_mirror_service.*` | 固定控制接口、会话设备与生命周期；后台操作通过快照返回，不依赖 Qt |
| 投屏适配与表现 | `src/adapters/device/scrcpy_backend.*`、`src/ui/screen_mirror_panel.*` | 适配器拥有 scrcpy 进程和固定 ADB 命令；Qt 只处理面板、调度和状态反馈 |
| 文件传输 | `src/core/device/file_transfer.h`、`src/application/device/file_transfer_service.*` | 文件值对象与接口、固定批次设备和路径、取消及逐项结果；无 Qt |
| 文件适配与表现 | `src/adapters/device/adb_file_transfer_backend.cpp`、`src/ui/pages/file_transfer_workspace.*` | 适配器拥有固定目录/传输/替换命令和临时目录；Qt 负责列表、选择、同名确认与后台调度 |

`DeviceSession`、`FeatureRegistry`、`JobCoordinator` 等名称只表示规划中的职责，
不是已经存在的类。新增代码应优先按职责补齐，不为匹配文档一次性创建空类型。

## 4. Windows 端职责

### 4.1 Presentation

- 显示当前设备、连接状态、设备服务兼容性和全局风险。
- 把用户意图转换为 application 用例，不拥有协议和测试业务状态机。
- 将异步领域状态映射到 Qt 控件，并只在 GUI 线程更新界面。
- 为复杂能力提供专用页面；界面模式不应通过隐式端口名称推断。

页面拆分必须先提取状态模型和用例，再按单串口与双端调试拆分视图；不能只把同一逻辑移动到更多
QWidget 类中。阶段完成度和剩余提取项见 `STATUS.md`。

### 4.2 Application

Application 层负责跨接口用例和生命周期，目标职责包括：

- 设备会话：把一个物理设备、ADB 状态、Agent 状态和能力快照关联起来。
- 串口会话：连接代次、端点配置、计划发送状态和断线收敛。
- AT 指令库：保存名称、正文和发送数据区的共享行尾，执行边界校验与本地持久化；不拥有串口连接或第二套发送队列。
- 任务协调：启动、取消、恢复、进度和资源冲突的上位机映射。
- 运行历史：保存参数、设备快照、结果摘要和产物索引。

当前 `AgentConnectionService` 集中处理重连、按需部署、协议握手、版本校验、能力和系统信息发现，
并在失败时回收端口转发。`MainWindow` 只负责把该同步用例放入后台任务，并将结果映射到 Qt 控件。
连接客户端的具体创建仍由现有工厂完成；只有出现第二种实际传输实现时才把装配继续外移。

`AdbTerminalService` 接收明确的设备快照并校验在线状态；ADB 适配器复用已有路径解析，
进程适配器启动独立系统终端。Qt 只负责后台调度与反馈，不等待交互式 Shell 的生命周期。

该层只能依赖 core 接口和值对象，不包含 Qt 或具体 ADB 命令。
投屏服务通过 `IScreenMirrorBackend` 注入适配器；默认构造入口 `createScreenMirrorService` 在桌面适配器中组合，
UI 不直接创建具体后端。子进程通过私有环境复用 ADB，关闭只针对其拥有的投屏进程。
文件服务通过 `IFileTransferBackend` 注入适配器，由适配器的 `createFileTransferService` 组合默认实现。
`prepare` 固定目标并重置取消标志，不执行 I/O；后台 `executePrepared` 顺序传输并逐项汇总。
目录读取有独立取消和界面代次，传输保持自己的设备快照；切换设备时旧目录回调不能覆盖新界面。
文件适配器只执行固定的 ADB `push/pull` 与目录、属性、暂存替换、新建、重命名、删除和经纯八进制校验的权限命令，所有路径独立引用；
不读取外部终端命令，不提供任意命令输入或 Agent shell RPC。暂存、冲突及权限边界见 [文件传输契约](features/FILE_TRANSFER.md)。
文件管理请求与传输共用 `FileTransferService` 的任务 ID、互斥和逐项结果。本地及设备固定变更由 `adapters/device/file_management` 执行，实际路径、类型、挂载点和不跟随链接的判断保留在执行层，界面确认不替代执行前检查。目录历史/收藏属于界面偏好，按设备限额持久化。

`DeviceMaintenanceService` 管理固定设备、动作、导出路径、超时、取消及分阶段结果；`IDeviceMaintenanceBackend` 由桌面 ADB 适配器实现。
适配器只执行固定的日志、Android 身份、Root/Remount、状态查询与重启命令；不接入 Agent RPC 或任意 Shell 参数。
日志标准输出通过受管进程的流式回调写同目录部分文件，成功后提交；标准错误独立限额，GUI 只读取字节和状态快照。
`MainWindow` 从文件、串口、投屏及服务准备状态建立同设备维护门禁，开始后同步阻止新冲突动作并使旧服务代次失效；各入口仍检查门禁，Windows COM 与本地文件管理独立工作。
窗口关闭取消并等待所属后台任务收尾，不终止共享 ADB server。详细边界由 [设备维护契约](features/DEVICE_MAINTENANCE.md) 定义。
`BoardTestJobClient` 继续只拥有请求构造、响应校验和快照。`BoardInfoService` 为三个固定只读信息项提供顺序读取、取消/超时、代次和观察值解码，不引入通用任务引擎或规格判定。
桌面在后台建立独立控制连接，避免硬件读取与主窗口断开/重连共用 socket；仅固定 `job.start/status/cancel`，不经过 ADB Shell。`BoardInfoPanel` 用 Qt 定时器显示快照并检查设备/会话代次，主窗口复用既有维护门禁与退出收尾。规则见 [硬件信息契约](features/BOARD_INFO.md)。

串口终端与普通监视器各有独立会话；RX 通过有界队列和合并通知，在 GUI 线程核对代次后送入各自显示模型。终端按连续流解析，
Qt 只把变化的行映射为样式并处理焦点、选区和输入法；按键输入通过 `SerialSession` 的异步队列。
终端只显示板端回显，不增加第二个串口连接或在普通日志上按时间推断帧边界。行为见
[串口终端契约](features/SERIAL_TERMINAL.md)。

`SerialSession` 拥有无界面依赖的 `SerialRecorder`，在显示投递之前有界缓存 RX，并记录传输报告成功的 TX。
录制器独占文件和工作线程；打开、写入、周期刷盘、元数据和关闭均不在串口回调或 GUI 线程执行。
显示队列满时淘汰旧数据，录制队列满则停止录制并报告不完整，二者分别统计，限额与行为见 [串口契约](features/SERIAL.md)。
正常关闭窗口先停止任务并异步等待录制收尾；底栏只聚合各用例快照和带任务 ID 的停止意图，不引入通用任务引擎。

### 4.3 Core

- 定义稳定值对象、传输接口、协议帧和可独立测试的状态转换。
- 错误使用稳定分类和面向人的上下文，不用日志文本前缀推断状态。
- 所有外部输入均校验长度、枚举、路径、版本和数值范围。
- 队列有容量、关闭和背压语义；不能把无限缓存当作可靠性。

## 5. 板端 Agent

Agent 使用 C++17 与平台 API 实现，不依赖 Qt。开发期默认只监听板端 `127.0.0.1`。Android 和 Linux
通过 ADB 动态端口转发；Linux 使用固定 Bundle 和受控版本目录部署。直接 TCP 必须由显式产品配置开启，并先
补充认证、授权和网络暴露策略。

Agent 产物由“操作系统用户空间 + ARM 架构”确定：Android 使用 NDK/Bionic 的 `armeabi-v7a` 或
`arm64-v8a`，固定构建矩阵的 Linux 第一阶段使用 glibc 2.31 基线的 `aarch64`。内核版本不作为
产物目录维度；更换内核后重新验证设备节点、驱动、termios、权限和安全策略。

Agent v1 的最小能力族包含会话握手、能力查询、系统信息、串口枚举、串口托管和静态注册的板级
能力；实际开放范围见 `STATUS.md`。扩展能力时按需要逐步加入：

- `SessionManager`：协议协商、心跳和会话恢复。
- `CapabilityRegistry`：稳定能力 ID、参数、风险、资源和版本。
- `JobManager`：任务状态、超时、取消和结果归一化。
- `ResourceManager`：UART、音频、摄像头和存储写入等资源裁决。
- `RunnerSupervisor`：隔离测试进程、管理进程组和输出限流。
- `ArtifactStore`：管理短期日志、报告、图片和校验信息。
- `PlatformAdapter`：隔离 Android、Buildroot 和通用 Linux 差异。

Agent 不提供任意 shell RPC。每项能力必须映射到受控实现或允许的测试程序，并声明参数、资源、
风险、超时和取消能力。

板级能力 ID 只表达稳定语义，Provider 身份只表达当前实现来源。静态 ProviderBinding 把能力定义、
可用性探测和固定执行函数绑定；边界只允许 `agent_native`、`runner_pack`、`sdk_bsp` 和可选
`rk_reference` 四类，不建立动态插件或任意路径执行。执行结果、能力专用 observations、上位机
Board Profile evaluation 和最终 verdict 分层处理，详细契约见
[`ADR-0003`](decisions/0003-stable-board-capabilities-and-providers.md)。

## 6. 协议与数据通道

控制连接使用长度前缀帧：

```text
4-byte big-endian payload length
1-byte frame kind
payload
```

控制 JSON 使用 UTF-8，请求至少包含协议版本、请求 ID、方法和参数。Agent 与 application 共享无 Qt、
有输入/深度/集合/字符串限额的 JSON 值模型，拒绝重复字段、非法 UTF-8、非整数数值和未知契约字段。
协议和串口数据通道定义以 [`AGENT_PROTOCOL_V1.md`](AGENT_PROTOCOL_V1.md) 为准。

串口等持续数据不与控制 JSON 混流：`serial.open` 返回只监听回环地址的临时数据端口，上位机通过
当前设备连接器建立独立隧道。未来任务事件和产物若共享控制连接，必须使用通道 ID、有界队列和公平调度；
结果与控制事件不能因日志流量被静默丢弃。

ADB `forward dev:/dev/...` 不属于产品串口路径。目标 Android 上的实机证据表明它会在连接后立即
EOF，即使节点权限满足也不能形成稳定数据流；产品统一通过 Agent 托管串口。

## 7. 任务、资源与能力

目标任务状态模型：

```text
created -> queued -> running -> succeeded
                         |----> failed
             |----------> cancelling -> cancelled
             |----------> interrupted -> recovering
```

任务 ID 独立于 TCP 连接。板端资源管理器是资源冲突的唯一裁决者，Windows 预检查只负责提前提示。
新增并发测试项前必须具备通用任务和资源管理；当前支持范围见 `STATUS.md`。

能力描述至少包含稳定 ID、显示名称、版本、参数约束、风险、权限、独占资源、默认超时、取消/恢复
支持、结果字段和产物类型。简单参数测试可使用通用运行页；串口、波形和图像通过同一能力 ID 绑定
专用页面，不能用自动表单覆盖所有交互。

## 8. 线程与生命周期

- GUI 线程只处理界面和轻量状态映射，不执行阻塞式 ADB、网络或串口操作。
- 每个长连接和线程有明确所有者；关闭顺序为停止新请求、取消读写、等待工作线程、释放回调。
- 回调携带会话或连接代次，过期结果不得覆盖新状态。
- 后台回调不能捕获无生命周期保护的 Qt 控件指针。
- 所有队列和等待都有容量或超时，析构不能依赖无限等待。

## 9. 部署与安全

- 开发期：Android 和 Linux 通过同一份版本化 Agent Bundle 部署和转发；Bundle v2 按 Android
  armeabi-v7a/arm64-v8a 或 Linux aarch64/glibc-2.31 选择目标包。每个包是安全相对 root 下的文件
  列表，当前只含 role=agent 文件，为未来 runner/config 保留扩展位。
- 稳定期：Android 使用受控 init/service 集成，Linux/Buildroot 使用 systemd 或包配置安装；ADB
  部署仍保留为开发与维修入口。
- 上位机与 Agent 独立版本，握手协商协议范围和能力版本。
- 升级失败不能破坏 ADB；需要保留版本查询、逐文件产物校验和 current/previous 回退入口。AgentVersion
  是整个包的唯一版本标识，SHA-256 只用于完整性校验。
- 当前安全依赖已有 ADB 授权和回环监听；网络认证、包签名和量产权限仍是开放项。

第一阶段内存容量和存储信息/健康由 Agent 原生读取 `/proc`、`/sys`，不依赖或启动原厂 PCBA server。
后续确需复用固件单项 Runner 时仍遵守 SDK/BSP 所有权和 Bundle ABI 边界，当前迁移边界见
[`ADR-0003`](decisions/0003-stable-board-capabilities-and-providers.md)。

## 10. 演进规则

- 先定义用户任务和功能契约，再决定 Qt 页面、application 用例和 Agent 能力。
- 先在 core/application 建立可测试状态，再连接 Qt；不得把新增业务状态继续堆入 QWidget。
- 新 Agent 能力先定义固定协议和安全边界，再实现部署与界面。
- 重大设计决定记录在 `docs/decisions/`；当前进度只写入 `STATUS.md`，历史证据写入 `DEVLOG.md`。
- 当前优先级、验证和交付状态不得写入本架构文档，以免稳定规则与动态事实混合。

## 11. UI 组件职责

主窗口装配导航、设备上下文和各工作区；串口模式由工作区校验，导航只在成功切换后同步选择。
普通调试和终端分别装配一个工作区实例，共享连接栏/接收组件实现，各自拥有 `SerialSession`、偏好和缓冲；通过共享端口登记避免同口重复打开。界面复用不共享活动连接或 RX 状态。
`FileListModel` 只保存条目与显示数据，排序/过滤代理不访问文件系统；`FilePane` 负责列表、目录历史、选择与位置恢复，
工作区拥有异步加载代次、固定目标和任务调度。`FileIconProvider` 按需后台请求 Windows Shell 图标并有界缓存，先返回回退图标；QImage 在线程间传递，QPixmap 与模型通知只在 GUI 线程创建/发出。绘制行时不阻塞查询设备或系统。
`SerialConnectionBar` 拥有连接参数控件，`ReceiveConsole` 拥有终端光标绘制与滚动反馈；会话、发送队列和计划发送仍由无 Qt 服务管理。
这些均是 `src/ui/widgets/` 内的表现组件，不成为 core/application 的依赖，也不复制业务状态机。

调试服务健康检查通过独立短超时连接执行已有只读协议，应用层验证握手、版本与串口能力，GUI 按连接代次接收结果。
失联清理撤销旧端口，ADB forward 清理在后台完成并阻止清理期间复用连接；本地串口保持独立。
