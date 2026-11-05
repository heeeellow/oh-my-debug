# TestTools Agent 协议 v1

[返回项目首页](../README.md)

## 当前实现

Agent 是运行在 ARM Android 或 ARM Linux 板端的无 Qt C++17 服务，控制协议不随系统和内核版本
改变。Agent 只监听板端 `127.0.0.1:27183`；Android 和 Linux 均通过 ADB 转发接入。当前产品已
实现 Android 和 Linux ADB 自动部署，Linux 固定选择 Bundle 中的 `aarch64/glibc-2.31` 产物。

当前程序版本和验证边界见 [`STATUS.md`](STATUS.md)。协议 v1 的仓库实现包括：

- `session.hello`：协议范围、Agent 版本和平台。
- `capability.list`：固定公布 `system.info` 与 `serial.stream`；运行时探测后公布
  `memory.capacity`、`storage.device.info` 和 `storage.device.health` 等静态注册能力。
- `system.info`：系统、内核、架构、板型、厂商/产品、Android 构建、CPU 核数、内存、SELinux
  状态和有效 UID。
- `serial.list`：设备节点、USB 接口名称、读写权限和占用进程。
- `serial.open` / `serial.close`：由 Agent 独占打开串口并分配临时原始数据端口。
- `job.start` / `job.status` / `job.cancel`：启动、查询或取消受控板级任务。
- `toolbox.info/prepare/abort`、`gpio.chip.list`、`network.interface.list`、`job.progress/renew`、
  `gpio.hold.extend`：工具箱实例、操作票据、元数据和所属任务控制，确切格式见下文。
- 强制协议版本、请求 ID、方法和串口参数类型、范围与枚举校验。

## 传输帧

```text
4-byte big-endian length   长度包含 kind 和 payload，不包含自身
1-byte kind                1 = UTF-8 JSON，2 = binary
payload                    当前最大 1 MiB
```

解码器支持 TCP 拆包和粘包，拒绝零长度、超限长度和未知帧类型。JSON 请求包含 `protocol`、
`request_id`、`method` 和 `params`，响应回显 `request_id`。

实现必须拒绝缺失或不支持的协议版本、重复/未知字段、错误字段类型、越界串口格式和未知枚举，不能
静默回退为另一组串口参数。Agent 和 application 客户端共享无 Qt 的严格 JSON 值模型；控制消息限制
为 1 MiB、有限嵌套和有限数组/对象成员，不接受浮点数或超出 int64 的整数。

## 串口数据通道

`serial.open` 校验 `/dev/tty*` 字符设备、检查占用、取得文件锁并配置 termios。成功响应包含
`session_id` 和仅绑定回环地址的 `stream_port`。上位机为该端口创建独立 ADB `tcp:` 转发，之后
在这条连接上直接传输串口原始字节；控制 JSON 不与高吞吐数据混流。

响应还包含 `baud_rate_requested`、Agent 文件描述符读取到的 `baud_rate_effective` 和
`baud_rate_applied`。对 `/dev/ttyUSB*`、`/dev/ttyACM*` 这类 USB 虚拟串口，该数值通常只是
termios 兼容字段，不代表 USB 总线或模块内部 UART 的物理速率，界面会按“驱动管理”提示。

当前实现一次只管理一个板端串口会话。界面会阻止同时打开两个 Agent 串口，但允许
“Windows 本地串口 + 板端 Agent 串口”的双端点验证。多板端并发将在会话表和资源管理器完成后开放。

## 板级任务

旧无参能力的 `job.start` 的 `params` 只包含能力列表已公布的 `capability_id`。成功后返回
`job-N` 形式的 `job_id`、能力 ID、`capability_version`、状态和结果；运行中结果为
`null`。`job.status` 和
`job.cancel` 只接受该受限 ID 形式，不接受路径或外部命令。

当前全局最多一个板级任务，状态为 `running` / `cancelling` / `succeeded` /
`failed` / `cancelled` / `timed_out`。运行态的 `result` 必须为 `null`；终态结果固定包含：

- `completion`：`completed/error/cancelled/timed_out`，必须与任务终态一致；
- `exit_code`；
- `provider`：`kind/id/version`，执行完成时必需；
- `observations`：`schema` 和能力专用的 `data` 对象，执行完成时必需；
- 可空 `provider_claim`、最多 1 KiB 摘要和最多 64 KiB 日志。

`completion=completed` 只表示能力执行完成，不等于最终业务通过。当前信息页面不进行规格判定。

启动与取消返回操作时在同一状态锁内取得的值快照；成功启动通常为 `running`，成功取消为
`cancelling`。后续完成或下一任务启动不改变已返回的响应，不依赖再次查询当前任务来生成响应。
`job.status` 仍只查询当前保留的任务，旧任务被下一任务替换后可返回 `job_not_found`。

取消与完成按状态锁内的提交顺序裁决：终态提交后再取消返回 `job_not_active`，不改写终态；取消先被
接受时，最终按 `cancelled` 收敛，即使 Provider 随后返回成功。保留已有优先级：Provider 明确报告的
超时优先于取消。`completion` 只从最终任务状态映射，不能另从 Provider 布尔值推断。
取消不表示设备操作已回滚；Provider 的退出码、观察值和日志保留实际证据，取消请求的回复也不代表
工作线程已退出。示例：

```json
{
  "completion": "completed",
  "exit_code": 0,
  "provider": {"kind": "agent_native", "id": "linux_proc_meminfo", "version": 1},
  "observations": {"schema": "memory.capacity/v1", "data": {"memtotal_kib": 3987528}},
  "provider_claim": null,
  "summary": "Observed Linux MemTotal",
  "log": ""
}
```

原有三个板级能力均无参数并由 Agent 原生只读 Provider 实现：`memory.capacity` 读取 `/proc/meminfo`；
`storage.device.info` 和 `storage.device.health` 只读取 `/sys`。它们不调用 `/data/echo_ddr_test`、
`/data/echo_emmc_test`，也不受原厂 PCBA server 常驻影响。完整能力模型与 Provider 边界见
[`ADR-0003`](decisions/0003-stable-board-capabilities-and-providers.md)。

## 工具箱扩展

沿用协议 1 的帧、请求信封和严格整数 JSON；旧无参能力/`job.status` 的响应不变。
工具能力的 `job.start` 使用下列参数及独立 `toolbox.job/v1` 响应，旧客户端不能将其当成旧任务启动响应。
桌面首先核对 AgentVersion、`toolbox.v1` 能力及 `toolbox.info/v1`，不向旧 Agent 发送新工具请求。
能力是否可执行还取决于运行时 GPIO 节点、权限和网口；清单注册不代表硬件已经验证。

### 实例、所有者与操作票据

`toolbox.info` 接受 `{}`，返回：

```json
{"schema":"toolbox.info/v1","instance_id":"0123456789abcdef0123456789abcdef","lease_ms":5000,"runner_available":true}
```

每次 Agent 启动生成新的 32 位小写十六进制 `instance_id`；桌面用同格式随机 `owner_id`。
所有者用于任务归属，不提供额外认证。除元数据和 info 外，以下方法均要求 owner/instance 匹配。

`toolbox.prepare` 接受 `{owner_id,instance_id}`，返回 `{operation_id,instance_id}`；
`operation_id` 由 Agent 签发，形式 `op-N`，30 秒内可首次启动。最多保留 32 个去重记录，
达到限额时回收未启动或已结束的旧记录；未知/已回收操作不能重放。

`toolbox.abort` 接受 `{owner_id,instance_id,operation_id}`，废弃票据；已启动时请求取消，
未启动时返回 `state=aborted` 且以后拒绝迟到启动。重复废弃可查询原记录。
启动回复丢失时先废弃并查询原操作，不能换新票据重发输出或脉冲。

### 元数据

| 方法 | params | 结果 |
| --- | --- | --- |
| `gpio.chip.list` | 可选 `chip:string`、`cursor:int=0`（0–65536）、`limit:int=128`（1–256） | `gpio.inventory/v1`；`chips,pins,total,next_cursor,complete,unavailable` |
| `gpio.board.list` | `{}` | `gpio.board.inventory/v1`；`endpoints,complete,unavailable`，最多 128 个端点，不读取电平 |
| `network.interface.list` | `{}` | `network.inventory/v1`；`interfaces` 数组 |

GPIO controller 字段为 `id,name,label,lines,accessible,linux_base`，失败含 `error`；pin 为
`chip,offset,name,consumer,direction,used,active_low,drive,abi,global_number`。`linux_base/global_number` 在实际 sysfs 设备身份匹配后提供，否则为 null；禁止从 gpiochip 序号猜基址。
v2 只读行探测返回 ENOTTY 或已知旧固件的 EINVAL 时，必须通过 v1 查询成功才采用 v1；权限错误不回退。
`cursor` 是所选范围的全局行偏移，
`total/next_cursor` 判定分页是否结束；`complete` 表示探测完整性，不能当成“本页即全部”。
枚举不请求行句柄。不接受任意设备路径，仅接受枚举所得 `gpiochipN`。

Agent 0.3.12 新增独立能力 `gpio.board.list/read/set`，桌面先核对能力，旧 Agent 不发送新方法。
板载端点包含 `endpoint_id,adapter,source=board,name,path,preference_key,driver,chip,offset,global_number,rockchip_name,controller_identity,used,consumer,direction,dt_flags,value_semantics,readable,writable,reason`。
`endpoint_id` 为当前注册表签发的 32 位小写十六进制标识；正常刷新稳定，节点/父目录、驱动设备、设备树及映射身份改变后失效。
`chip` 未确认时为空，offset/global_number 为 null。used 保留驱动占用；writable 独立表示公司接口能否设置，不能放宽原生申请限制。
`path` 仅用于观测，不接受作为请求参数；适配与用途限制见工具箱契约。

网络接口包含 `id,ipv4,up,physical,loopback,operstate,mac,speed_mbps,duplex` 和
`rx_bytes,tx_bytes,rx_errors,tx_errors`；未知数值为 `null`，不能补零。枚举可包含不可测速接口，
开始前仍须检查物理以太网、up/operstate、实际地址及选定路由。

### 执行与白名单参数

工具启动接受且只接受：

```json
{"owner_id":"0123456789abcdef0123456789abcdef","instance_id":"0123456789abcdef0123456789abcdef","operation_id":"op-1","capability_id":"gpio.pin.read","arguments":{"chip":"gpiochip4","offset":15}}
```

| capability_id | arguments（全为必需字段） |
| --- | --- |
| `gpio.pin.read` | `chip:string,offset:int(0–4095)` |
| `gpio.pin.write` | 同上，加 `level:int(0/1),hold_sec:int(1–300),drive:string` |
| `gpio.pin.pulse` | 同上，加 `idle_level:int(0/1),pulse_level:int(0/1),pulse_ms:int(10–10000),drive:string`，两电平相反 |
| `gpio.edge.monitor` | 同上，加 `edge:rising/falling/both,duration_sec:int(1–300)` |
| `gpio.board.read` | `endpoint_id:string(32位小写十六进制),adapter:rocktech-gpio/v1或rk-gpio/v1` |
| `gpio.board.set` | 同上，加 `level:int(0/1)`，物理输出；Agent 转换为驱动命令 10/11，无 hold/drive/路径参数 |
| `network.iperf3.server` | `interface:string,bind_ip:IPv4,peer_ip:IPv4,port:int(5201–5210),duration_sec:int(1–60),protocol:tcp/udp,parallel:int(1–8),udp_bps:int(1000000–1000000000)` |

`drive` 固定为 `push_pull/open_drain/open_source`；界面只开放已实施的推挽选项。
UDP 强制 `parallel=1`。IPv4 使用十进制四段，拒绝前导零、0/8、127/8 和组播/保留高段。
参数存在不代表实际内核支持，驱动或硬件拒绝必须保留失败原因。

板载读/设置沿用 prepare、job.start、owner/instance、操作去重、单任务和复核，硬期限 3 秒、所有者租约 5 秒。
设置为持续状态的一次写入，不随任务到期自动恢复；取消后不重试写入，也不发送相反电平。只读用途和身份不符在板端拒绝。
终态 observations 含 `endpoint,before_value,physical_level`（可读时）、`persistent,write_attempted,write_completed`；
设置另含 `target_level,driver_command,direction_verified`（执行到对应步骤时）。write_completed 只表示完整提交字节，任务成功还要求读回、方向和身份匹配。
读回或后续核对失败仍返回已有 observations；缺失字段不补零。服务重启后旧任务失败，重新读值只确认当前状态，不反推旧写入或恢复成功。

同操作/同参数重复启动返回原任务进度，不重复执行；参数改变返回 `operation_conflict`。
与旧信息任务共用一个 Agent 任务位，冲突立即拒绝，不排队执行。GPIO 由内核独占行句柄裁决。
工具启动/取消的进度响应与旧无参任务响应不同，不放宽旧请求的未知字段规则。

### 进度、租约与结束

`job.progress` 接受 `{owner_id,instance_id,operation_id|job_id,cursor?}`，两个 ID 必须恰好一个。
游标是非负整数，默认 0，超前游标拒绝。响应固定包括：

```json
{"schema":"toolbox.job/v1","instance_id":"0123456789abcdef0123456789abcdef","operation_id":"op-1","job_id":"job-1","capability_id":"gpio.pin.read","state":"running","active":true,"complete":true,"stage":"starting","detail":"","data":{},"events":[],"next_cursor":0,"gap":false,"remaining_ms":3000,"result":null}
```

`stage/data/detail` 是独立运行快照；`result` 在活动时为 null，终态含
`summary,log,exit_code,schema,observations`。状态沿用旧任务状态，另有未启动 `prepared/aborted`；
未启动的 `job_id/capability_id` 为空。未启动废弃票据没有执行结果。
事件包含递增 `sequence`，每批最多 256；重复游标可重读本批，确认游标可释放更旧事件。
队列最多 4096，落后/溢出时 `gap=true` 或 `complete=false`，不得宣称记录完整。

`job.renew` 和工具 `job.cancel` 只接受 `{owner_id,instance_id,job_id}`。
续租返回 `{renewed:true}`，取消返回原任务进度；旧的裸 job_id 取消不能绕过工具所有者检查。
续租只刷新 5 秒所有者租约，不改变操作硬期限，进度查询不续租。
`gpio.hold.extend` 在同样身份字段上加 `hold_sec:int(1–300)`，仅活动输出可用，
返回 `{remaining_ms}`。取消或已过期任务不能续租/延长，终态优先按任务锁裁决。
`job_not_active` 可能是完成与取消/续租竞争，客户端继续读取权威终态，不将其直接当成失联。

期限使用包含休眠时间的板端时钟。网络总期限为每方向时长+20 秒；读取 3 秒；
脉冲宽度+3 秒；保持/监听使用所选时长。租约或硬期限到达后关闭所属资源，禁止复活。
GPIO 正常保持/监听到期是完成，所有者租约丢失则超时。

GPIO observation 包含原方向、ABI、active-low、逻辑/物理数字值（可读取时）、事件计数、
时间基准、缺口、`released` 和 `release_state=uncontrolled`；后者不承诺电平恢复。
网络 observation 包含 `metrics,peer_ip,local_ip,cleaned,server_log_truncated`，
CPU 指标 `local_cpu_permille/remote_cpu_permille` 相对于输出该份记录的进程；板端记录的 local 指板卡。
只执行固定同目录 iperf3，不接收命令或路径。就绪检查只检查所属监听，不建立预连接。
iperf 小数由独立解析器转换为整数 bps/微秒/ppm/CPU 千分比，控制 JSON 本身仍拒绝小数。

主要错误：`invalid_params,stale_owner,operation_expired,operation_aborted,operation_conflict,resource_busy,`
`capability_unavailable,inventory_failed,job_not_active,lease_expired,unknown_method`。
断线或记录丢失不能证明已释放；客户端保留未知状态，复核原操作或新的实例身份后再放开门禁。
用户侧动作、报告限额及验收见 [工具箱契约](features/TOOLBOX.md)。

## 安全边界

- Agent 控制和串口数据端口只绑定回环地址；网络测速的固定 Runner 仅在显式开始任务后绑定所选以太网地址。
- ADB 只负责发现、部署和端口转发，不承担串口业务语义。
- 不提供任意 shell RPC；每个新能力必须有固定方法、参数校验和风险等级。
- `/dev/ttyUSB1` 等被系统进程占用的端点会返回明确占用者，不会抢占系统服务。
- Android 和 Linux 开发部署依赖已有 ADB 授权。两端均从 Bundle v2 选择匹配目标包并逐文件校验；Android 部署会请求并校验 root adbd，保证 Agent
  以有效 UID 0 运行后才允许连接；自动重连只读检查已有 Agent 身份，不隐式提权。Linux 部署要求
  设备已有可写的 root adbd。部署会校验 Bundle 清单和 SHA-256。量产阶段仍需补充包签名和
  init/systemd 服务集成。
- 上位机以 `session.hello.agent_version` 判断整个板端程序包是否与当前源码配套。后台设备发现只读握手；
  用户点击“连接并同步 Agent”时，版本不一致才自动部署当前产物。任何改变 Agent 行为、能力或返回
  字段，或改变随包部署的 Runner/配置的修改，都必须同步递增
  `src/core/protocol/agent_version.h` 中的 `AgentVersion`。SHA-256 和构建标识只用于完整性、目录隔离
  与诊断，不参与是否升级的判断。

## 请求示例

```json
{"protocol":1,"request_id":"hello-1","method":"session.hello","params":{}}
```

```json
{"protocol":1,"request_id":"serial-open-1","method":"serial.open","params":{"endpoint":"/dev/ttyUSB2","baud_rate":115200,"data_bits":8,"parity":"none","stop_bits":1,"flow_control":"none"}}
```

协议测试夹具可通过 CMake 选项 `TESTTOOLS_BUILD_FAKE_AGENT=ON` 单独构建；它不是最终用户功能。
