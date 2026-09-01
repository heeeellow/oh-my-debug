# TestTools 当前状态

[返回项目首页](../README.md)

更新日期：2026-09-01

本页是当前实现、验证、交付和开放项的唯一汇总。历史过程和旧结论见 [`DEVLOG.md`](DEVLOG.md)，
稳定产品与架构规则分别见 [`PRODUCT.md`](PRODUCT.md) 和 [`ARCHITECTURE.md`](ARCHITECTURE.md)。

## 1. 当前基线

- 分支：`master`
- 产品版本：`0.1.0`，由 Git 标签 `v0.1.0` 标记首个可交付开发基线。
- 交付状态：`v0.1.0` 已在本地创建标签；当前 `master` 继续包含 Linux Agent 自动部署、发布运行
  修复和 Android Agent root 身份修复，均未推送到远端。
- 当前产品入口：根目录 `TestTools.slnx`，其中只包含 Qt GUI 工程。
- 当前板端路径：Windows 可通过 ADB 自动部署并连接 Android Agent；Linux `aarch64/glibc-2.31`
  Agent 已可由固定矩阵构建并生成 Bundle，当前 GUI 已接入 Linux ADB 自动部署、版本切换和连接。
- 当前 Agent：协议版本 1，程序版本 `0.2.2`；只支持一个板端串口会话。

## 2. 已实现能力

- ADB 设备发现、Agent ABI 识别、按需部署、启动、控制端口转发和现有 Agent 重连。
- Agent `session.hello`、`capability.list`、`system.info`、`serial.list`、`serial.open` 和
  `serial.close`。
- Windows 本地串口与板端串口的连接、文本/HEX 收发、TX/RX 显示和字节统计。
- 普通定时/次数发送、双向验证和基于序号/CRC 的可靠性测试。
- 串口工作台默认单串口调试，并可显式切换双端调试；双向验证和可靠性压测是双端调试的按需子功能。
- 通用串口提供文本/HEX 和显式行尾选择，端口名称不改变发送语义。
- UI 页面快照、自检、Agent 部署/重连、无发送串口保持和显式 AT 冒烟入口。
- Application 已承接串口工作区模式/资源冲突规则、具体传输创建边界、短任务生命周期和单端
  `SerialSession`。会话统一拥有传输、连接代次、异步发送队列、收发统计和普通计划发送；异步断开
  不在 GUI 线程等待 Agent 数据通道退出。
- Agent 对协议版本、串口字段类型、范围和枚举执行拒绝式校验；命令行端口解析不再因非法输入异常
  终止。
- Agent 产物目标只接受 ARM：Android 为 `armeabi-v7a/arm64-v8a`，固定矩阵的 Linux 为
  `aarch64/glibc-2.31`；UI 通过应用层连接服务创建客户端，Android/ADB 部署类使用明确的平台命名。
- ADB Agent 部署门面按 Android/Linux 用户空间选择部署器；Linux 部署校验 Bundle manifest、远端
  SHA-256、aarch64、glibc >= 2.31 和 root adbd，并使用版本目录、`current`/`previous`、固定日志
  与 PID 路径管理启动和回退。
- Android Agent 部署和停止会请求并复核 root adbd；自动重连只读核验已有 Agent 身份。Android
  连接只接受有效 UID 0 的 Agent，避免普通 `shell` 用户无法打开 `radio:radio` 串口节点。
- 仓库提供 UTF-8/LF/格式规范和 `scripts/check-baseline.ps1` 统一检查入口。

串口两种调试方式和轻量视觉基线已经实现。可靠性压测复用双端配置并使用独立状态与确定性帧；
结构化结果页、任务历史和可导出报告仍待通用任务模型落地。

## 3. 验证状态

| 范围 | 最近证据 | 状态与边界 |
| --- | --- | --- |
| GUI 启动 | 2026-08-29，主解决方案只启动 Qt GUI | 已验证；记录于 `DEVLOG.md` |
| 基线静态检查 | 2026-09-01，当前工作树 | 文档链接、差异空白、Qt 边界、UI 适配器边界、工程文件完整性和生成物跟踪检查通过 |
| Windows Release 构建 | 2026-09-01，标准输出目录 | 编译和链接通过；Qt 运行时与平台插件、Android Agent `armeabi-v7a` 同步生成；仅有可选 Direct3D 12 DLL 未找到提示 |
| Windows Debug 构建 | 2026-09-01，标准输出目录 | 编译和链接通过；Qt 运行时与平台插件、Android Agent `armeabi-v7a` 同步生成；仅有可选 Direct3D 12 DLL 未找到提示 |
| Core/Application 主机测试 | 2026-09-01，CTest | 1/1 通过；覆盖串口会话和 ARM-only 产物映射，不依赖 Qt、ADB 或串口设备 |
| Agent 主机契约测试 | 2026-09-01，CTest | 1/1 通过；覆盖版本、字段类型、枚举和范围拒绝路径，不访问设备 |
| Agent 固定矩阵与 Bundle | 2026-08-31，统一入口首次及重复运行 | Android 两 ABI、Linux aarch64 均 Release 构建通过；build id 和三个产物 SHA-256 重复运行稳定；manifest 独立校验通过 |
| Linux ADB Agent 自动部署 | 2026-08-31，Nexus4 ADB 设备 `976ccefbf0c7e61b` | 自动识别 Ubuntu/aarch64/glibc 2.35、选择 glibc-2.31 Bundle、远端 SHA-256、版本目录切换、启动、真实 PID、ADB 转发和四项协议只读冒烟均通过；已清理临时部署 |
| Android ARM Agent 构建 | 2026-08-31，NDK 27.3.13750724 Release | `armeabi-v7a` 为 ELF32/ARM，`arm64-v8a` 为 ELF64/AArch64；两者均通过矩阵构建 |
| Linux ARM Agent 构建 | 2026-08-31，WSL2 + Debian Bullseye 固定工具链 | ARM64 ELF，动态依赖含 `libc.so.6`，最高 GLIBC 符号 `2.17`，不高于 2.31 |
| Linux Agent 临时实机验证 | 2026-08-31，Nexus4 ADB 设备 `976ccefbf0c7e61b` | 设备实际为 Ubuntu 22.04.5/aarch64、glibc 2.35；临时版本目录部署 SHA-256 一致，`session.hello`、`capability.list`、`system.info`、`serial.list` 均通过；已清理，不含串口读写 |
| 当前串口界面 | 2026-08-31，Release 构建 | 1366x768 单串口/双端、1920x1080 单串口快照通过；未打开串口或发送数据 |
| Release 自包含启动与 ADB 发现 | 2026-08-31，清理 Qt 环境变量的 Release 进程 | `TestTools.exe` 直接启动成功，同目录 Qt DLL/平台插件有效，发现 2 台 ADB 设备并连接现有 Agent；未部署、未打开或写入串口 |
| 内置自检 | 2026-09-01，当前 Release 产物 | 通过；识别在线 ADB 设备，仅执行安全转发，不发送串口数据 |
| Android Agent 0.2.2 部署 | 2026-09-01，设备 `e2ea0f728463744a` | 自动切换 root adbd，Agent 有效 UID/GID 均为 0；部署、重连、`system.info` 和 `serial.list` 通过，未发送串口数据 |
| Agent 非法端口参数 | 2026-08-30，同一设备 | `--port invalid` 确定返回退出码 2，未异常崩溃 |
| `/dev/ttyUSB2` 数据通道 | 2026-09-01，经 root Agent 打开并保持 5 秒 | 实机通过；未发送任何字节 |
| `/dev/ttyUSB2` AT 收发 | 2026-08-31，用户通过当前主界面复测 | 实机通过；AT 指令收到板端返回 |

验证状态与交付状态分开记录。旧实机结果只能证明当时的设备和产物，不自动升级为当前工作树的
回归结果。

## 4. 当前设计与技术债务

- `SerialWorkspace` 已不再持有串口连接实例、发送线程、普通计划或字节统计，但仍协调双向验证与
  可靠性压测；这些跨端点流程应在通用任务模型确定后下沉，不为减少 QWidget 行数提前制造空层。
- Core、Application 和具体适配器已按职责分目录；`SerialWorkspace` 仍是较大的 Qt 页面实现，后续拆分
  必须继续先下沉状态和用例，不能只移动 QWidget 代码。
- 可靠性压测已集成到双端调试，且与普通发送保持独立状态；结果仍主要显示在顶部摘要，尚无完整结构化结果页。
- 完整 AT 指令库、响应分组和 URC 识别尚未实现。
- Agent 只有单串口会话，控制 JSON 仍使用受限字段读取而非完整结构化解析；还缺少连接上限、资源表、
  心跳、断线恢复、认证和正式 Android service 集成。
- Linux 固定矩阵和 glibc 2.31 Bundle 已完成；Linux ADB 部署与 GUI 连接已接入，systemd 服务和包签名仍未实现。
- Windows Release 目录已包含 Qt 运行时，可直接启动；正式安装包、签名和升级分发仍未实现。
- 测试中心、运行记录和多数板级测试仍是产品骨架，尚未形成完整任务与产物闭环。
- 历史代码仍混用 `serialdemo` 与 `testtools` 命名空间；不影响行为，但应在独立机械重构中统一。
- 当前没有 CI 和 clang-tidy 门禁；统一脚本和格式文件已建立本地可执行基线。

## 5. 下一阶段顺序

1. 在更多 Linux ARM 板卡上验证 ADB 部署、版本回退、串口枚举和服务运行，再评估 systemd 管理与
   包签名；Android 继续使用 ADB 连接器。
2. 补充 `/dev/ttyUSB2` 计划发送和断开响应的主界面回归；AT 收发已经通过。
3. 用结构化 JSON 请求模型和稳定错误分类加固 Agent，再扩展连接上限、资源表、心跳和断线恢复；
   在此之前不开放多板端串口并发。
4. 在新增并发板级测试前定义通用任务、资源、结果和产物契约，并下沉双向验证/可靠性压测协调。
5. 任务模型稳定后接入完整 AT 工具、首批真实板级测试、结构化结果和运行记录。
