# TestTools Agent 协议 v1

[返回项目首页](../README.md)

## 当前实现

Agent 是运行在安卓或 Linux 板端的无 Qt C++17 服务。上位机通过 ADB 把它部署到
`/data/local/tmp/testtools-agent/`，Agent 只监听板端 `127.0.0.1:27183`，控制和数据端口都通过
ADB 转发到 Windows。本机假 Agent 只保留为协议测试夹具，不出现在产品界面，也不参与正常构建。

当前程序版本和验证边界见 [`STATUS.md`](STATUS.md)。协议 v1 的仓库实现包括：

- `session.hello`：协议范围、Agent 版本和平台。
- `capability.list`：`system.info` 与 `serial.stream`。
- `system.info`：系统、内核、架构、板型、系统版本和有效 UID。
- `serial.list`：设备节点、USB 接口名称、读写权限和占用进程。
- `serial.open` / `serial.close`：由 Agent 独占打开串口并分配临时原始数据端口。
- 强制协议版本、请求 ID、方法和串口参数类型、范围与枚举校验。

## 传输帧

```text
4-byte big-endian length   长度包含 kind 和 payload，不包含自身
1-byte kind                1 = UTF-8 JSON，2 = binary
payload                    当前最大 1 MiB
```

解码器支持 TCP 拆包和粘包，拒绝零长度、超限长度和未知帧类型。JSON 请求包含 `protocol`、
`request_id`、`method` 和 `params`，响应回显 `request_id`。

实现必须拒绝缺失或不支持的协议版本、错误字段类型、越界串口格式和未知枚举，不能静默回退为
另一组串口参数。协议解析保持有界输入；完整 JSON 结构化解析属于后续协议加固项。

## 串口数据通道

`serial.open` 校验 `/dev/tty*` 字符设备、检查占用、取得文件锁并配置 termios。成功响应包含
`session_id` 和仅绑定回环地址的 `stream_port`。上位机为该端口创建独立 ADB `tcp:` 转发，之后
在这条连接上直接传输串口原始字节；控制 JSON 不与高吞吐数据混流。

响应还包含 `baud_rate_requested`、Agent 文件描述符读取到的 `baud_rate_effective` 和
`baud_rate_applied`。对 `/dev/ttyUSB*`、`/dev/ttyACM*` 这类 USB 虚拟串口，该数值通常只是
termios 兼容字段，不代表 USB 总线或模块内部 UART 的物理速率，界面会按“驱动管理”提示。

当前实现一次只管理一个板端串口会话。界面会阻止同时打开两个 Agent 串口，但允许
“Windows 本地串口 + 板端 Agent 串口”的双端点验证。多板端并发将在会话表和资源管理器完成后开放。

## 安全边界

- Agent 和数据端口只绑定回环地址，不直接暴露到板卡网络。
- ADB 仅负责发现、部署和端口转发，不承担串口业务语义。
- 不提供任意 shell RPC；每个新能力必须有固定方法、参数校验和风险等级。
- `/dev/ttyUSB1` 等被系统进程占用的端点会返回明确占用者，不会抢占系统服务。
- 当前开发部署依赖已有 ADB 授权。量产阶段仍需补充包签名、版本校验和 init/service 集成。

## 请求示例

```json
{"protocol":1,"request_id":"hello-1","method":"session.hello","params":{}}
```

```json
{"protocol":1,"request_id":"serial-open-1","method":"serial.open","params":{"endpoint":"/dev/ttyUSB2","baud_rate":115200,"data_bits":8,"parity":"none","stop_bits":1,"flow_control":"none"}}
```

协议测试夹具可通过 CMake 选项 `TESTTOOLS_BUILD_FAKE_AGENT=ON` 单独构建；它不是最终用户功能。
