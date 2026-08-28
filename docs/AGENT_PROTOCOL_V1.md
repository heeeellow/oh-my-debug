# TestTools Agent 协议 v1 雏形

## 当前目的

这一版用于验证“上位机负责交互与编排、板端服务负责受控设备操作”的边界。它不是可直接进入
量产系统的远程管理服务。目前只实现协议编解码、`session.hello`、`capability.list` 和一个仅监听
本机回环地址的假 Agent。

## 传输帧

```text
4-byte big-endian length   长度包含 kind 和 payload，不包含自身
1-byte kind                1 = UTF-8 JSON，2 = binary
payload                    当前最大 1 MiB
```

解码器支持 TCP 拆包和粘包，拒绝零长度、超限长度和未知帧类型。JSON 请求必须包含协议版本、
`request_id`、`method` 和 `params`。响应回显 `request_id`，业务任务后续使用独立的 `job_id`。

## 已实现的最小请求

```json
{"protocol":1,"request_id":"hello-1","method":"session.hello","params":{}}
```

```json
{"protocol":1,"request_id":"caps-1","method":"capability.list","params":{}}
```

假 Agent 只声明只读的 `system.info` 能力，用于证明能力发现链路。它不执行 shell，也不开放真实
硬件操作。

## 安全和部署边界

- 假 Agent 只绑定 `127.0.0.1`；真实板端第一阶段也应通过 ADB 转发访问，不监听外网接口。
- 不提供任意 shell RPC。新能力必须有稳定 ID、参数约束、风险等级和资源声明。
- 当前假服务没有认证、持久任务、超时、取消和恢复机制，不得作为生产服务部署。
- 当前 `AgentClient` 是同步冒烟客户端，只能在后台工作线程中使用；接入 GUI 前需增加超时与异步
  会话管理。

## 本机验证

构建 `test/test.slnx` 后会同时生成 `test.exe` 和 `testtools-agent.exe`。先启动假 Agent：

```powershell
.\test\x64\Release\testtools-agent.exe --port 27183
```

再从另一终端运行上位机协议冒烟：

```powershell
.\test\x64\Release\test.exe --agent-smoke 127.0.0.1 27183
```

验证成功后假 Agent 会在客户端断开时退出。

`test/agent/CMakeLists.txt` 只依赖 C++17、Asio 和线程库，可作为后续 Android/Linux 交叉编译入口；
当前目标仍是假服务，不包含 init/service 配置或真实板卡能力。
