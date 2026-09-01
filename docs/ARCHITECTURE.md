# TestTools 总体架构

[返回项目首页](../README.md)

## 1. 架构目标

TestTools 从串口助手演进为设备中心的调试与测试工作台。架构需要同时保证：

- Qt 限于 Windows 表现层和 GUI 线程桥接，核心逻辑可在没有 Qt 的环境复用和测试。
- ADB 是可替换的发现、部署和隧道通道，不承载设备能力的业务语义。
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
| Application | `src/application/{agent,device,serial,support}/` | 设备发现、部署、串口工作区状态、传输创建边界和后台任务所有权 |
| Core | `src/core/{protocol,serial,support}/` | 协议、值对象、接口、编解码、发送与测试逻辑；标准 C++，不得依赖 Qt |
| Host adapters | `src/adapters/{adb,agent,serial}/` | ADB、进程、Agent TCP 和 Windows COM 的具体实现；不保存产品用例状态 |
| Board Agent | `src/agent/agent_service.*`、`serial_bridge.*` | 固定能力、参数校验和板端资源访问 |
| 通用任务/历史/产物 | 规划中的 application/core 职责 | 不为匹配架构图创建空类型 |

`DeviceSession`、`FeatureRegistry`、`JobCoordinator`、`HistoryStore` 等名称只表示规划中的职责，
不是已经存在的类。新增代码应优先按职责补齐，不为匹配文档一次性创建空类型。

## 4. Windows 端职责

### 4.1 Presentation

- 显示当前设备、连接状态、Agent 兼容性和全局风险。
- 把用户意图转换为 application 用例，不拥有协议和测试业务状态机。
- 将异步领域状态映射到 Qt 控件，并只在 GUI 线程更新界面。
- 为复杂能力提供专用页面；界面模式不应通过隐式端口名称推断。

页面拆分必须先提取状态模型和用例，再按单串口与双端调试拆分视图；不能只把同一逻辑移动到更多
QWidget 类中。阶段完成度和剩余提取项见 `STATUS.md`。

### 4.2 Application

Application 层负责跨接口用例和生命周期，目标职责包括：

- 设备会话：把一个物理设备、ADB 状态、Agent 状态和能力快照关联起来。
- 串口会话：连接代次、端点配置、计划发送状态和断线收敛。
- 任务协调：启动、取消、恢复、进度和资源冲突的上位机映射。
- 运行历史：保存参数、设备快照、结果摘要和产物索引。

该层只能依赖 core 接口和值对象，不包含 Qt 或具体 ADB 命令。

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

Agent v1 的最小能力族包含会话握手、能力查询、系统信息、串口枚举和串口托管；实际开放范围见
`STATUS.md`。扩展能力时按需要逐步加入：

- `SessionManager`：协议协商、心跳和会话恢复。
- `CapabilityRegistry`：稳定能力 ID、参数、风险、资源和版本。
- `JobManager`：任务状态、超时、取消和结果归一化。
- `ResourceManager`：UART、音频、摄像头和存储写入等资源裁决。
- `RunnerSupervisor`：隔离测试进程、管理进程组和输出限流。
- `ArtifactStore`：管理短期日志、报告、图片和校验信息。
- `PlatformAdapter`：隔离 Android、Buildroot 和通用 Linux 差异。

Agent 不提供任意 shell RPC。每项能力必须映射到受控实现或允许的测试程序，并声明参数、资源、
风险、超时和取消能力。

## 6. 协议与数据通道

控制连接使用长度前缀帧：

```text
4-byte big-endian payload length
1-byte frame kind
payload
```

控制 JSON 使用 UTF-8，请求至少包含协议版本、请求 ID、方法和参数。协议和串口数据通道定义以
[`AGENT_PROTOCOL_V1.md`](AGENT_PROTOCOL_V1.md) 为准。

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

## 10. 演进规则

- 先定义用户任务和功能契约，再决定 Qt 页面、application 用例和 Agent 能力。
- 先在 core/application 建立可测试状态，再连接 Qt；不得把新增业务状态继续堆入 QWidget。
- 新 Agent 能力先定义固定协议和安全边界，再实现部署与界面。
- 重大设计决定记录在 `docs/decisions/`；当前进度只写入 `STATUS.md`，历史证据写入 `DEVLOG.md`。
- 当前优先级、验证和交付状态不得写入本架构文档，以免稳定规则与动态事实混合。
