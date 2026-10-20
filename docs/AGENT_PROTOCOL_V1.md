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

`job.start` 的 `params` 必须包含能力列表已公布的 `capability_id`。成功后返回
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

`completion=completed` 不等于最终业务通过。Board Profile 和 `PASS/VERIFY/FAIL` 判定在上位机完成，
不写回 Agent 快照。

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

当前三个板级能力均无参数并由 Agent 原生只读 Provider 实现：`memory.capacity` 读取 `/proc/meminfo`；
`storage.device.info` 和 `storage.device.health` 只读取 `/sys`。它们不调用 `/data/echo_ddr_test`、
`/data/echo_emmc_test`，也不受原厂 PCBA server 常驻影响。完整能力模型与 Provider 边界见
[`ADR-0003`](decisions/0003-stable-board-capabilities-and-providers.md)。

## 安全边界

- Agent 和数据端口只绑定回环地址，不直接暴露到板卡网络。
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
