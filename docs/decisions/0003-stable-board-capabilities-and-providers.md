# ADR-0003：稳定板级能力模型与 Provider 迁移边界

[返回总体架构](../ARCHITECTURE.md)

- 状态：已接受
- 日期：2026-09-02
- 历史状态：第一阶段 M1-M3 曾完成验证；M4 和高风险能力未实施。Board Profile 与测试中心不属于当前产品入口，现行能力见 [协议](../AGENT_PROTOCOL_V1.md) 和 [设备信息契约](../features/BOARD_INFO.md)。

## 1. 背景与问题

2026-09-02 的验证版本已具备统一的 `job.start/status/cancel`、任务终态、超时、取消、日志上限和测试中心
界面。首个试点 `rk.pcba.ddr.capacity` 通过固定参数调用 RK 固件内的 `/data/echo_ddr_test`，证明了任务
链路，但它同时暴露了三个不适合继续扩展的问题：

1. 能力 ID 绑定了 RK PCBA 实现，而不是描述稳定的测试语义。
2. 容量读取本来只需要 `/proc/meminfo`，却会因原厂 `echo_pcbatest_server` 常驻而不可用。
3. 当前结果是字符串 map，执行是否完成、观察值、板型期望和最终判定仍混在一起。

补丁 `0004` 中的“DDR 测试”实际包含容量、两种固定频率压力、两种动态调频压力、挂起恢复、
自刷新 CRC 和重启共八种不同风险的动作。补丁 `0005` 中的“eMMC 测试”则把设备信息、健康、顺序
读写性能、MD5 完整性和清理绑定到一次会写存储的执行中。它们不能继续作为两个粗粒度能力接入。

2026-09-02 的只读实机探测还确认：RK3572 Buildroot 与 RK3562 Android 都可直接读取
`/proc/meminfo`；两板的非可移动 eMMC 也都在 `/sys/class/block` 暴露容量、型号、厂商、固件和
寿命字段。因此首阶段没有必要依赖原厂 PCBA server 或写存储来获得这些观察值。

## 2. 决策目标与非目标

本次设计必须：

- 保留协议 v1 的任务方法、任务 ID、状态、取消、超时和现有界面框架。
- 让能力 ID 表达测试语义，Provider 只表达实现来源。
- 明确分离 Agent 执行、原始观察、Board Profile 评估和最终 verdict。
- 建立足够承接 Agent 原生、Runner Pack、SDK/BSP 和 RK Reference 的最小边界。
- 第一批只实现读取型能力，且不受原厂 PCBA server 常驻影响。
- 为高风险能力写清资源、参数硬边界和恢复条件，但不在本阶段执行它们。

本次明确不做：

- 不设计 `dlopen`、脚本插件、目录扫描或第三方动态注册系统。
- 不提供任意程序路径、shell、环境变量或 Runner 参数透传。
- 不建立通用规则语言、可执行表达式或自动生成所有界面的表单框架。
- 不实现多任务并发、持久任务历史、事件推送或跨重启恢复框架。
- 不执行存储写入、DDR 压力、DMC 调频、挂起、自刷新、重启或清理原厂状态。

## 3. 稳定能力模型

### 3.1 能力定义

每个能力由 Agent 内静态注册表定义，至少包含：

- `id`：稳定语义 ID，不含供应商或可执行文件路径；
- `version`：该能力请求、观察结果和行为契约的版本；
- `provider`：当前选中的实现来源、实现 ID 和实现版本，仅用于诊断；
- `risks`：固定枚举集合；
- `resources`：固定资源集合；
- `parameters`：允许字段、类型、范围和默认值；
- `observation_schema`：能力专用的结构化观察 schema；
- `default_timeout_ms`、`supports_cancel` 和 `recovery`。

能力定义不携带任意命令、路径或动态 schema。客户端也不能通过参数选择 Provider。

### 3.2 三层结果

结果按以下顺序形成，三层不能互相替代：

```text
Provider 执行
  -> Execution：是否完成、取消、超时或执行错误
  -> Observations：设备实际观察值和有界证据
  -> Board Profile Evaluation：观察值与该板型期望的逐项比较
  -> Verdict：PASS / VERIFY / FAIL
```

具体规则：

- `completion=completed` 表示 Provider 完成并产生了符合 schema 的观察值，不等于测试通过。
- 启动失败、解析失败、取消或超时保留现有任务终态，`verdict` 为空，界面显示“执行失败/已取消/超时”。
- Provider 发现硬件不符合测试条件但成功产出结构化 finding 时，任务仍可为 `succeeded`，由评估层给出
  `FAIL`，不能把硬件失败伪装成运行器异常。
- 原厂 Runner 输出的 `PASS/VERIFY/FAIL` 只能记录为 `provider_claim` 或原始证据，不直接成为最终 verdict。
- `PASS`：唯一匹配的 Board Profile 存在，且所有必需检查都通过。
- `FAIL`：执行成功，且至少一个必需检查明确失败。
- `VERIFY`：执行成功，但没有唯一 Profile、缺少必需期望/观察值，或规则明确要求人工核对。

### 3.3 协议包络

继续使用协议 v1 和现有 `job.*` 方法。实现阶段把终态 `result.fields` 字符串 map 迁移为以下结构，
并通过共享的无 Qt JSON 模型解析和生成：

```json
{
  "completion": "completed",
  "provider": {
    "kind": "agent_native",
    "id": "linux_proc_meminfo",
    "version": 1
  },
  "observations": {
    "schema": "memory.capacity/v1",
    "data": {
      "memtotal_kib": 3987528
    }
  },
  "provider_claim": null,
  "summary": "Observed Linux MemTotal",
  "log": ""
}
```

共同包络稳定，`observations.data` 使用能力专用的固定 schema。此设计不引入任意 key/value 指标语言，
也不要求不同领域强行共享同一数据结构。

Board Profile 评估发生在无 Qt 的 Windows application/core 代码中；Agent 不接收板型阈值，也不因
上位机 Profile 改变安全边界。评估结果随当前运行记录保存，不回写 Agent 任务快照。

## 4. Board Profile

Board Profile 是随上位机版本管理的只读 JSON 数据，建议位于 `profiles/boards/`。它只包含：

- `profile_id` 和 `profile_version`；
- 对 `system.info` 中 OS、厂商、产品、板名等字段的精确匹配条件；
- 已定义领域的类型化期望，例如内存 `memtotal_kib` 范围、存储类型/容量和健康上限；
- 高风险能力以后允许的参数范围，但 Agent 仍必须保留独立硬上限。

不支持正则脚本、表达式求值、继承链或 Profile 间覆盖。零个或多个 Profile 匹配都返回 `VERIFY`；
只有唯一匹配才执行自动判定。评估结果必须记录 Profile ID、版本和逐项 check，保证结论可追溯。

首阶段不能根据本轮实测的 `3987528 KiB` 或 `2011208 KiB` 反推板型期望。未获得各 SKU 的审核期望前，
`memory.capacity` 仍显示真实观察值并给出 `VERIFY`，不能把“接近 4 GiB/2 GiB”硬编码为通过。

## 5. 最小 Provider 边界

Provider 不采用动态插件。Agent 使用静态 `ProviderBinding` 表，把能力定义、可用性探测和执行函数绑定：

```text
ProviderBinding
├── CapabilityDefinition definition
├── ProviderIdentity identity
├── probe() -> available / unavailable + reason
└── run(validated request, cancel token) -> execution + observations
```

四种 Provider 的边界如下：

| Provider | 来源与部署 | 允许行为 | 兼容性依据 |
| --- | --- | --- | --- |
| `agent_native` | 编译进通用 Agent | 直接使用受审查的平台 API、`/proc`、`/sys` | Agent 版本、OS/架构和运行时探测 |
| `runner_pack` | 独立版本包，随 Agent Bundle 或单独受控部署 | 只能运行 manifest 中固定 entry ID；无路径和参数透传 | Pack schema、版本、目标 ABI、文件摘要和能力声明 |
| `sdk_bsp` | 随 SDK/BSP/rootfs 构建安装 | 调用固件提供的固定能力入口或受控接口 | 固件能力标记、suite/ABI/BSP 约束 |
| `rk_reference` | 可选 RK 原厂兼容适配器 | 解析固定 `echo_*` 子项，保留原厂占用检查；不能启动 PCBA server | 严格 RK 标记、Runner 版本、固定参数和结果解析版本 |

每个能力在代码中给出明确的 Provider 候选顺序，不建立全局自动优先级。一次能力发现只公布一个实际
Provider；切换 Provider 不改变能力 ID，但观察 schema 或行为变化时必须递增能力版本。

`rk_reference` 是迁移后端，不是长期产品协议。它可以临时继承已验证的原厂算法，但原厂进程常驻、
原始协议和路径不会泄漏到上位机能力 ID。

## 6. 能力拆分

### 6.1 DDR 八项迁移

| 原厂子项 | 稳定能力 ID | 风险 | 资源 | 参数与恢复要求 | 首阶段 |
| --- | --- | --- | --- | --- | --- |
| capacity | `memory.capacity` | `read_only` | `memory.inventory` | 无参数；无恢复 | 实现，Agent 原生 |
| fixed_stressapptest | `memory.ddr.stressapptest.fixed` | `stress` | `memory.ddr`、`cpu.system`、`thermal` | 时长硬上限；取消后确认进程组退出 | 仅定义 |
| fixed_memtester | `memory.ddr.memtester.fixed` | `stress` | `memory.ddr`、`cpu.system`、`thermal` | 大小/循环硬上限；释放分配并确认进程退出 | 仅定义 |
| dynamic_stressapptest | `memory.ddr.stressapptest.dynamic` | `stress`、`clock_control` | `memory.ddr`、`dmc.clock`、`cpu.system`、`thermal` | 记录并恢复 governor/频点，恢复失败单独上报 | 仅定义 |
| dynamic_memtester | `memory.ddr.memtester.dynamic` | `stress`、`clock_control` | 同上 | 同上，并释放内存 | 仅定义 |
| suspend_resume | `memory.suspend_resume` | `power_transition` | `system.suspend`、`rtc.wakeup` | 次数/保持时间硬上限；恢复后重新握手并核对周期 | 仅定义 |
| self_refresh_crc | `memory.self_refresh_crc` | `stress`、`power_transition` | `memory.ddr`、`system.suspend`、`rtc.wakeup` | 释放缓冲区；恢复后校验 CRC 和周期 | 仅定义 |
| reboot | `system.reboot.cycle` | `reboot` | `system.lifecycle`、`persistent.test_state` | 明确用户确认；重连续跑；只清理本任务拥有的状态 | 仅定义 |

`memory.capacity` 直接读取 `/proc/meminfo` 的 `MemTotal`，返回精确 `memtotal_kib`。它不调用
`echo_ddr_test`，不检查、停止或抢占 `echo_pcbatest_server`，因此原厂服务常驻不再影响该能力。

### 6.2 eMMC/UFS 拆分

| 稳定能力 ID | 内容 | 风险 | 资源 | 参数与恢复要求 | 首阶段 |
| --- | --- | --- | --- | --- | --- |
| `storage.device.info` | 类型、内核名、容量、厂商、型号、固件 | `read_only` | `storage.inventory` | 无参数；只枚举非可移动整盘，排除分区/boot/RPMB/dm/loop/zram | 实现，Agent 原生 |
| `storage.device.health` | eMMC/UFS pre-EOL、寿命字段和只读 I/O 计数 | `read_only` | `storage.inventory` | 无参数；字段缺失作为观察缺失，不伪造 0 | 实现，Agent 原生 |
| `storage.performance.sequential` | 受控顺序读写吞吐 | `storage_write` | `storage.write`、`page_cache` | 目标使用受控设备 key；大小/空间硬上限；删除临时文件并核对清理 | 仅定义 |
| `storage.integrity.roundtrip` | 写入、回读和摘要比对 | `storage_write` | `storage.write` | 固定工作目录；大小硬上限；摘要与清理分别出结果 | 仅定义 |

补丁 `0005` 的 `echo_emmc_test` 每次都会进入 benchmark 和 MD5 写入路径，没有只读子项选择，因此不得
作为首阶段 info/health Provider。首阶段只移植并重新审查其 `/sys` 发现思路，不执行原厂程序，不访问
可写挂载点，不调用 `drop_caches`，也不创建报告或进度文件。

## 7. 第一阶段迁移范围

设计获准后，按以下顺序实现；每一步保持现有单任务模型，不并发推进高风险能力。

### M1：共享 JSON 与能力包络

- 在 `src/core/protocol/` 增加一份有输入/深度/字符串/数组上限的 JSON 值模型和解析/生成测试。
- Agent 请求解析与 application 响应解析改用同一模型，删除继续扩展字符串查找解析器的需要。
- `job.start/status/cancel` 方法、任务 ID 和状态不变。
- `BoardTestRunResult` 改为结构化 execution/provider/observations/evidence，不再用字符串 map 承载领域数据。

### M2：静态 Provider 注册与原生只读能力

- 增加静态能力定义和 `ProviderBinding`，由 `AgentService` 遍历注册表生成 `capability.list` 并分派任务。
- 实现 `memory.capacity`、`storage.device.info`、`storage.device.health` 三个 Agent 原生 Provider。
- 所有外部字段拒绝式校验；三项能力均无客户端参数，不接受路径或设备名注入。
- 原厂 PCBA 进程占用检查仅保留在 `rk_reference` 适配器，不影响原生只读能力。

### M3：Board Profile 与界面迁移

- 在无 Qt application/core 层加载和唯一匹配 Board Profile，产出逐项 checks 和 verdict。
- 测试中心沿用现有页面框架，显示能力、运行状态、观察值、Profile 判定和证据；不生成任意参数表单。
- 把未提交试点 `rk.pcba.ddr.capacity` 直接迁移为 `memory.capacity`，不同时公布旧/新两个 ID。
- 删除 UI 和 application 对 RK 容量字段、原厂 verdict 和 `original_pcba_busy` 的特殊分支。
- 实现时 Agent 版本从已验证的 `0.3.5` 递增到 `0.3.6`，不得复用已部署版本号。

### M4：高风险能力（未实施）

- 第一阶段通过后，仅补齐 Runner Pack manifest 和 SDK/BSP/RK Reference 的静态契约测试夹具。
- 任何压力、写存储、调频、挂起或重启能力都必须另行审核参数上限、设备前置条件和恢复演练，不能因
  Provider 边界存在就自动开放。

## 8. 兼容与回退

- 协议仍为 v1；新 Agent 通过程序版本和能力版本协商，不修改现有帧格式或任务方法。
- `rk.pcba.ddr.capacity` 尚未形成已提交发布基线，因此不保留运行时别名，避免永久兼容债务。
- 早期 `rk_pcba_runner` 试点代码与 `integrations/rk-pcba` 已从工作树移除，不再进入构建、Bundle 或能力发现。
- 若原生实现验收失败，回退到当前工作树的 PCBA 容量试点，不把失败实现部署到设备。
- RK3572 SDK、Buildroot、`external/rk_pcba_test` 和 `device/rockchip` 在第一阶段均不需要修改。

## 9. 验收门槛

### 9.1 主机契约

- JSON 模型覆盖截断、深度、重复字段、错误类型、超大整数、非法 UTF-8 和响应上限。
- Provider 注册覆盖重复能力 ID、不可用原因、固定分派、未知能力和参数注入拒绝。
- 观察 schema 覆盖缺字段、错误类型、多设备和有界证据。
- Board Profile 覆盖零匹配、唯一匹配、多匹配、PASS、VERIFY、FAIL 和版本记录。
- 现有成功、取消、超时、资源忙、任务 ID、析构清理和日志限流测试继续通过。

### 9.2 构建与界面

- Agent 三目标固定矩阵、Bundle v2、Windows Debug/Release、Core/Application 和 Agent CTest 全部通过。
- 1366x768 与 1920x1080 快照检查三项只读能力的可用、运行、结果、VERIFY/FAIL 和异常路径。
- 结束前运行 `scripts/check-project-docs.ps1` 和基线检查，并复核差异只覆盖批准范围。

### 9.3 双板只读实机

- RK3572 Buildroot 与 RK3562 Android 都通过 `memory.capacity`，观察值与直接只读证据一致。
- 两板都能枚举 eMMC 信息与健康字段；字段不可用时明确为缺失，不能当作 0 或 PASS。
- RK3572 的原厂 PCBA server 保持常驻且 PID/启动状态不被 TestTools 改变，三项能力仍可运行。
- 验收期间不出现 `echo_ddr_test`/`echo_emmc_test` 子进程，不创建存储临时文件，不写报告/进度文件，
  不调用 cache drop，不停止服务，不执行压力、调频、挂起、重启或清状态。

## 10. 初期实施范围

初期迁移采用以下范围：

1. 接受语义能力 ID 与 Provider 身份分离，并迁移旧容量 ID。
2. 接受首批只实现三个 Agent 原生只读能力。
3. 接受 Board Profile 无唯一匹配时统一给 `VERIFY`，不从实测值反推期望。
4. 接受高风险七项 DDR 和两项存储写测试另行逐项审核，不在第一阶段开放。
