# ADR-0007：前置架构清理、原生感终端引擎、GPIO 快速控制与 iperf3 网速工具规划

[返回当前状态](../STATUS.md) · [产品设计](../PRODUCT.md) · [界面设计](../UI_DESIGN.md) · [总体架构](../ARCHITECTURE.md) · [设备协议](../AGENT_PROTOCOL_V1.md) · [上一轮方案（ADR-0006）](0006-workstation-ux-and-capabilities-plan.md)

- **日期**：2026-11-03
- **状态**：阶段 P0 已完成并提交本地基线（`0.14.3`）；当前实现、验证与交付统一见 [STATUS](../STATUS.md)。T1/T2、GPIO、iperf3 与视觉精修仍为后续阶段。
- **核心目标**：
  1. **前置仓库清理与架构解耦（阶段 P0）**：清理历史构建残留与未接入产品的死代码，将耦合在 3350 行 [`SerialWorkspace`](../../src/ui/pages/serial_workspace.cpp) 中的串口终端拆分为独立页面组件，收敛常驻定时器与 UI 测试构建配置，为新终端引擎扫清结构障碍。
  2. **终端交互与渲染重塑（阶段 T1 / T2）**：解决现有串口终端刷屏卡顿、复制粘贴不顺手、富文本框排版缺乏原生终端感的问题；引入成熟 VT 引擎 `libvterm` 与字符网格自绘控件，并将 ADB Shell 演进为应用内多标签终端。
  3. **板级实用工具箱（阶段 G+N：GPIO + iperf3）**：在保持“Agent 无任意 shell RPC、所有能力固定 ID 与参数校验、所有依赖随包离线分发”的前提下，新增兼容 Android（64/32 位）与 Linux（aarch64）的 GPIO 快速读写/边沿监听，以及 PC 到板卡以太网的 iperf3 一键测速。
  4. **UI 风格与布局精修后置（阶段 U）**：等终端与新增工具页面结构稳定后，再统一优化全局色彩令牌、卡片间距与按钮样式，避免中途返工。
- **验证分工**：桌面构建、无 Qt 核心测试、主机夹具、Bundle v2 校验与界面快照由开发侧完成；开发板实机验证由用户执行，未完成实机验证前在 [`STATUS.md`](../STATUS.md) 中明确标记“未验证”。

---

## 1. 背景与痛点分析

以下是 P0 前 `0.14.2` 基线的使用反馈与代码体检，不能作为整理后的当前状态。该基线工作台在架构可维护性与高频调试场景下存在四个突出问题：

1. **串口终端与单/双串口工作区强耦合，且仓库存在历史残留**：
   - 主窗口的“串口终端”页面目前通过 [`MainWindow::buildSerialPage(true)`](../../src/ui/main_window.cpp) 创建**第二个 [`SerialWorkspace`](../../src/ui/pages/serial_workspace.cpp) 实例**（单文件已达 3351 行），在内部隐藏通道 A、发送框与快捷指令侧栏，仅借用 `channels_[1]` 运行终端。这导致终端实例在后台冗余创建了双通道控件、双向验证、压测状态机与常驻 10ms 定时器，且无法自然演进为“串口 + 内嵌 ADB Shell”多标签结构。
   - 仓库内留有早期未接入产品的死代码（如 `board_profile.*`）、废弃的 `TestToolsAgent.vcxproj`，以及超过 4.27 GB 的旧版迭代构建缓存与散落 `.obj` 文件。
2. **终端体验未能达到日常主力终端（Windows Terminal / Tabby）的流畅度与手感**：
   - **刷屏卡顿**：现有 [`ReceiveConsole`](../../src/ui/widgets/serial_receive_view.h) 基于 `QPlainTextEdit`，[`SerialWorkspace::renderTerminal`](../../src/ui/pages/serial_workspace.cpp) 每次刷新通过 `QTextCursor` 修改 `QTextDocument` 富文本块并重建 `ExtraSelections`。在内核启动日志、`dmesg` 或高波特率连续输出下，GUI 线程频繁承担文档排版与高亮重算，导致界面卡顿。
   - **复制粘贴不顺手**：现有契约将 `Ctrl+C` 固定为发送 `0x03`，复制必须按 `Ctrl+Shift+C`，且缺少双击选词、三击选行、矩形块选、选中即复制/右键粘贴、自动折行长行还原与多行粘贴安全确认。
   - **显示效果欠佳**：`QTextDocument` 的行高、字距、CJK 双宽字符对齐、Box Drawing 制表框线及反色光标并非严格字符网格，且缺少备用屏幕（Alternate Screen Buffer），无法正常操作 `vim`、`top`、`htop`、`menuconfig`。此外，ADB Shell 仍需弹出外部系统终端窗口，与串口终端割裂。
3. **缺少板端硬件引脚（GPIO）的快速观测与控制入口**：
   - 硬件联调常需拉高/拉低使能脚（如外设电源、复位脚、LED）或观测按键/中断输入电平变化；目前需切到外部终端手敲 `/sys/class/gpio` 或 `gpioset` 命令，操作繁琐且易误触内核已占用引脚。
4. **缺少一键以太网吞吐测速（iperf3）**：
   - 板卡以太网验收常需测试 PC ↔ 板卡的上下行吞吐，手动在两端起停 `iperf3`、核对网卡 IP 与协商速率耗时较长，且需要所有二进制随程序打包、开箱即用。

---

## 2. 阶段 P0：前置仓库清理与架构解耦方案

在引入 `libvterm` 和新功能前，先执行一轮**零外部行为变化的工程清理与模块拆分**，集中修改后统一构建与回归；提交、打标签和推送仍按独立授权执行。

### 2.1 仓库与构建残留清理清单

#### A. 未跟踪残留与历史构建产物（按文件内容清理，保留验证证据）

| 清理目标 | 当前占用 | 清理规则 |
| --- | --- | --- |
| 仓库根目录 `json_value.obj`、`json_value_smoke.obj` | 0.6 MB | 早期命令行编译遗留在根目录的目标文件，直接删除 |
| `build/iteration-0.2.0` ～ `build/iteration-0.14.1`、`build/review-*`、`build/ui-*`、`build/adr-*`（共 22 个旧目录） | 约 4.25 GB | 仅清理已核对可重建的二进制和编译中间文件；保留日志、报告、快照、修改前备份与目录结构，当前证据和活动构建目录不清理。实际清单与释放空间见 STATUS |
| `src/agent/obj/`、`src/agent/out/`、`src/agent/x64/` | 21.5 MB | 早期在 `src/agent/` 目录内直接生成的文件；现已统一由 `build/agent/` 承接，直接删除 |
| 根目录 `PATCHES/`（可选，按需保留） | 104.6 MB | 原厂 RK PCBA 补丁参考包，本轮保留不动 |

#### B. 已废弃或未接入产品的跟踪源码清理

| 目标文件 | 现状问题 | 处理方式 |
| --- | --- | --- |
| `src/application/agent/board_profile.h`<br>`src/application/agent/board_profile.cpp`（原基线约 770 行） | 为早期“板型规格 JSON 比对与 PASS/VERIFY/FAIL 自动判定”编写；按 2026-11-01 用户决策，设备信息页只做只读展示、不做规格判定，当前产品代码零引用，仅测试文件仍在编译 | 从源码、[`TestTools.vcxproj`](../../TestTools.vcxproj)、[`TestTools.vcxproj.filters`](../../TestTools.vcxproj.filters)、[`tests/CMakeLists.txt`](../../tests/CMakeLists.txt) 与 [`tests/core_application_tests.cpp`](../../tests/core_application_tests.cpp) 中移除（Git 历史可随时追溯） |
| `src/agent/TestToolsAgent.vcxproj`<br>`src/agent/TestToolsAgent.vcxproj.user` | 早期 Windows 本地调试 Agent 的 VS 工程；当前本机契约测试与三端交叉编译均以 [`src/agent/CMakeLists.txt`](../../src/agent/CMakeLists.txt) 为唯一入口，保留会导致新增模块需双写工程文件 | 删除该 `.vcxproj` 及 `.user`，同步更新 [`docs/DEVELOPMENT.md`](../DEVELOPMENT.md) 第 1 节说明 |

### 2.2 代码架构前置解耦（P0-1 ～ P0-4）

```text
拆分前：
MainWindow
├── serialWorkspace_   -> SerialWorkspace (3351 行，含 Single + Dual + Terminal + 快捷指令弹窗 + 双端验证/压测 + 常驻 10ms 定时器)
└── terminalWorkspace_ -> SerialWorkspace (第二个完整实例，隐藏通道 A 和发送区，仅用 channels_[1] 跑终端)

拆分后（阶段 P0）：
MainWindow
├── serialWorkspace_   -> SerialWorkspace (仅负责单串口与双端调试，按需启动定时器)
│   ├── AtCommandDialog      (src/ui/widgets/at_command_dialog.*：独立快捷指令新增/编辑/管理弹窗)
│   └── SerialLinkVerifier   (src/application/serial/serial_link_verifier.*：无 Qt 双端验证与压测调度状态机)
└── terminalWorkspace_ -> SerialTerminalWorkspace (src/ui/pages/serial_terminal_workspace.*：独立终端页，通过 SerialEndpointWorkspace 复用 SerialConnectionBar + SerialSession + SerialPortClaims + SerialRecorder)
```

1. **P0-1：剥离独立的 [`SerialTerminalWorkspace`](../../src/ui/pages/) 页面组件**
   - 新建公共端点宿主 `SerialEndpointWorkspace`，把实际连接、异步释放、显示投递和录制的共用规则集中；终端与普通调试各自持有需要的端点，避免复制生命周期实现。新建 `src/ui/pages/serial_terminal_workspace.h/.cpp`，仅包含单端点连接栏（复用 [`SerialConnectionBar`](../../src/ui/widgets/serial_connection_bar.h)）、单个 [`SerialSession`](../../src/application/serial/serial_session.h)、共享端口登记 [`SerialPortClaims`](../../src/application/serial/serial_port_claims.h)、终端显示视图、搜索栏、录制与关键字高亮入口，以及 `terminal-preferences.json` 读写。
   - 从 [`SerialWorkspace`](../../src/ui/pages/serial_workspace.cpp) 中移除 `WorkspaceMode::Terminal` 分支（`buildTerminalOptions`、`handleTerminalKey`、`renderTerminal`、`terminalRenderTimer_` 等约 450 行代码），使 `SerialWorkspace` 只聚焦 `Single` / `Dual` 调试模式。
   - **收益**：消除第二个 `SerialWorkspace` 实例的冗余控件与后台定时器，并为阶段 T1 替换 `TerminalGridWidget`、阶段 T2 扩展多标签（串口 + ADB Shell）提供独立、干净的宿主容器。
2. **P0-2：拆分 `SerialWorkspace` 内部弹窗与双端链路测试协调逻辑**
   - 将快捷指令管理弹窗提取至 `src/ui/widgets/at_command_dialog.h/.cpp`；
   - 将双端收发验证与可靠性压测的帧生成、方向状态机与速率推算收敛至 `src/application/serial/serial_link_verifier.h/.cpp`，提升无 Qt 层可测性并把 `serial_workspace.cpp` 缩减至约 2400 行以内。
3. **P0-3：收敛空闲定时器与精度**
   - 将 `SerialWorkspace::scheduleTimer_`（10ms）改为**仅在存在活动计划发送或可靠性压测任务时启动，任务结束立即停止**，不再在应用空闲时每秒唤醒 100–200 次；
   - 启用 `Qt::PreciseTimer` 保持 10ms 调度精度；等待压测尾部数据时停止发送调度，已有尾部超时回调继续有效。
4. **P0-4：加固 UI 测试构建与目标文件依赖**
   - 在 [`tests/ui/CMakeLists.txt`](../../tests/ui/CMakeLists.txt) 中按产品工程清单确定对象，工程变化触发重配置；[`scripts/check-baseline.ps1`](../../scripts/check-baseline.ps1) 构建前调用对象清理脚本，防止文件拆分或重命名后链接到陈旧符号；
   - 同步更新 `tests/ui/` 中访问终端页面的测试入口，保持现有 11 项原生回归与全部无 Qt 契约测试 100% 通过。

---

## 3. 终端模式重塑方案（分 T1 / T2 两步推进）

### 3.1 技术选型：`libvterm` + 字符网格自绘控件（`TerminalGridWidget`）

| 候选方案 | 架构与体验评估 | 结论 |
| --- | --- | --- |
| **`libvterm`（纯 C，MIT）+ Qt 字符网格自绘** | 标准 C 实现完整 VT220/xterm 状态机、主/备屏幕、滚动区（scroll region）、宽字符与鼠标报告；不依赖 Qt，可直接放入 `src/application/serial/`，完全符合无 Qt 分层边界；UI 层继承 `QAbstractScrollArea` 按可见行列与脏区（damage）绘制，吞吐高、内存可控 | **采用** |
| `xterm.js` + WebView2 / Qt WebEngine | 视觉接近 Tabby，但引入浏览器运行时与跨进程 JS 桥接，包体积、内存占用和离线分发复杂度显著增加 | 不采用 |
| `QTermWidget` | GPL 许可证且强耦合 POSIX/Linux 伪终端，Windows 适配薄弱 | 不采用 |
| 继续修补现有 `SerialTerminalBuffer` + `QPlainTextEdit` | 无法从根本上消除 `QTextDocument` 重排开销，补齐备用屏幕和复杂 CSI 的维护成本高于引入成熟库 | 不采用 |

### 3.2 T1：串口终端引擎替换与原生交互手感

#### 1. 渲染与高吞吐防卡顿架构
- **后台解析与双缓冲快照**：串口接收字节先进入录制队列（保持录制独立性），随后在后台终端工作线程喂入 `libvterm` 并更新屏幕网格与回滚环形缓冲（默认 20000 行上限）；GUI 线程不执行 VT 解析。
- **帧率解耦（Frame Coalescing）**：后台产生脏行（damage）时只置位原子通知标志；GUI 按固定帧预算（约 16ms / 60Hz）拉取最新可见视口快照并仅调用 `viewport()->update(dirtyRect)`。当串口以 1.5 Mbps 连续刷屏时，自动跳过中间帧、直接渲染最新一屏，保证主界面零卡顿、按钮与键盘即时响应。
- **严格字符网格渲染**：
  - 预计算等宽字体（`Cascadia Mono` / `Consolas`）固定单元格尺寸 `cellWidth × cellHeight`，ASCII 占 1 列、CJK 宽字符占 2 列。
  - `U+2500..U+257F` 制表框线（Box Drawing）与块元素采用 `QPainter` 几何自绘，确保 `menuconfig`/`htop` 边框严丝合缝、不随字体行距断裂。
  - 完整支持 16 标准色、256 色、24-bit TrueColor、粗体、斜体、下划线、反色，以及块状（Block）、竖线（Bar）、下划线（Underline）光标和窗口失焦空心框光标。
  - 默认提供原生感深色终端配色（如 One Dark / Campbell 柔化版），同时在“终端外观”中提供暖白浅色与高对比配色、字号（11–18px）及行高微调，偏好持久化至 `terminal-preferences.json`。

#### 2. 复制粘贴与键盘交互（对齐 Windows Terminal / Tabby）
- **智能 `Ctrl+C` / `Ctrl+V`（默认开启，可切回传统模式）**：
  - **当终端存在非空文本选区时**：按 `Ctrl+C` 复制选中文本到剪贴板并清除选区，**不向串口发送 `0x03`**；
  - **当终端无选区时**：按 `Ctrl+C` 向串口发送 `0x03`（SIGINT 中断）；
  - `Ctrl+Shift+C` 始终复制，`Ctrl+V` / `Ctrl+Shift+V` / `Shift+Insert` 始终粘贴；
  - 在“终端选项”中保留“Ctrl+C 始终发送中断”开关，兼顾不同使用习惯。
- **鼠标选择与快捷复制粘贴**：
  - 支持单击定位、拖动字符级选择、**双击按词边界选词**、**三击选整行**、**`Alt + 鼠标拖动` 矩形列块选择**；
  - 软折行（Auto-wrapped）的连续长行在复制时自动还原为单行，硬换行才插入换行符；
  - 提供可选开关“**选中即复制**”与“**右键直接粘贴**（无选区时右键粘贴，或右键菜单一键操作）”；
  - **多行粘贴保护**：粘贴内容包含换行符时，弹出紧凑预览确认框（显示行数与前几行内容，可勾选本次会话不再提示），支持 Bracketed Paste Mode（板端开启 `?2004h` 时自动包裹 `ESC [ 200 ~ ... ESC [ 201 ~`）。
- **串口窗口尺寸（Rows × Cols）边界**：
  - 物理 UART 没有带外窗口大小通知机制。为严守“不隐式向串口注入未授权命令字节”的工程边界，终端缩放时不自动发送命令，而是在终端状态栏实时显示当前网格尺寸（如 `120 × 36`），并提供一键复制/填入 `stty rows 36 cols 120` 的便捷入口。

### 3.3 T2：内嵌 ADB Shell 与多标签终端工作区

- 在 T1 串口终端稳定后，将 `SerialTerminalWorkspace` 升级为**多标签（Tabbed）终端工作区**：
  - 支持在标签栏点击 `+` 新建**串口终端标签**或**当前在线设备的内置 ADB Shell 标签**；
  - 内置 ADB Shell 通过后台受管伪终端/交互进程接入同一个 `libvterm + TerminalGridWidget` 组件，支持多开不同或相同设备的 Shell 会话；每个标签固定启动时的设备序列号，设备切换或断线独立显示状态；
  - 远程调试页仍保留“打开外部系统终端 ↗”作为回退入口；
  - **原型先行核验项**：在 Windows 上对比 ConPTY 包装 `adb -s <serial> shell` 与管道模式下的全屏程序（`vi`/`top`）及窗口大小动态同步行为，实测通过后再定型 T2 默认通道。

---

## 4. GPIO 快速控制与观测方案（兼容 Linux aarch64 与 Android 64/32 位）

### 4.1 平台接口选型：Linux GPIO 字符设备 ABI v1（`/dev/gpiochip*`）

- 摒弃已废弃且在现代内核常被关闭的 `/sys/class/gpio` sysfs 接口，也**不依赖板端是否预装 `gpiod`（`gpioinfo`/`gpioset`）命令行工具**。
- 由板端 Agent 使用 C++17 直接调用 Linux 标准头文件 `<linux/gpio.h>` 的 **Character Device ABI v1** `ioctl`（Linux 4.8+ 引入，Android 11 / Kernel 4.19、Android 32/64 位及 Rockchip Linux 5.10/6.1 均原生支持）：
  - `GPIO_GET_CHIPINFO_IOCTL`、`GPIO_GET_LINEINFO_IOCTL`：枚举控制器与每根引脚状态；
  - `GPIO_GET_LINEHANDLE_IOCTL`、`GPIOHANDLE_GET_LINE_VALUES_IOCTL`、`GPIOHANDLE_SET_LINE_VALUES_IOCTL`：读取输入电平或驱动输出电平；
  - `GPIO_GET_LINEEVENT_IOCTL` + `poll`/`read`：监听输入引脚上升沿/下降沿中断事件（注意处理 32 位 `armeabi-v7a` 与 64 位结构体的对齐兼容性）。
- 三套目标产物（`android-arm64-v8a`、`android-armeabi-v7a`、`linux-aarch64`）共用同一份无外部依赖的 Agent 原生实现。

### 4.2 能力定义与断开保持语义

| 能力 ID | 风险等级 | 资源锁 | 参数与行为 |
| --- | --- | --- | --- |
| `gpio.chip.list` | `read_only` | `gpio.inventory` | 无参数或指定 `chip`；返回所有 `/dev/gpiochipN` 的行数、每根引脚的 `offset`、名称、方向（输入/输出）、是否有效低电平、是否被内核驱动占用（`used`）及占用者名称（`consumer`） |
| `gpio.pin.read` | `read_only` | `gpio.line:<chip>:<offset>` | 指定 `chip` 与 `offset`，以输入模式请求句柄并读取当前逻辑电平（`0` / `1`）后立即释放 |
| `gpio.edge.monitor` | `read_only` | `gpio.line:<chip>:<offset>` | 指定 `chip`、`offset`、触发边沿（`rising` / `falling` / `both`）与持续时长上限（如最长 300s）；持续记录边沿事件与内核时间戳，支持随时取消 |
| `gpio.pin.write` | `hardware_control` | `gpio.line:<chip>:<offset>` | 指定 `chip`、`offset`、输出电平（`0` / `1`）与保持策略；在任务/持有期间保持该输出句柄打开以维持电平，用户点击“释放引脚”或 Agent 会话断开时自动关闭句柄释放引脚 |
| `gpio.pin.pulse` | `hardware_control` | `gpio.line:<chip>:<offset>` | 指定 `chip`、`offset`、脉冲电平与脉宽毫秒数（范围 `10..10000 ms`），输出脉冲后恢复反向电平或释放句柄（常用于外设 Reset 脚复位） |

- **断开后电平语义**：Linux `/dev/gpiochip` 字符设备的设计是进程持有 fd 期间占用并驱动引脚，关闭 fd 后内核自动释放引脚。本方案采用**“持有期间维持电平，手动释放或断开连接后自动释放句柄”**的确定性模型，并在界面上清晰展示当前处于“正在保持输出（持有中）”的引脚列表与一键释放按钮。

### 4.3 安全边界与界面交互

1. **禁止抢占内核已占用引脚**：若 `GPIOLINE_FLAG_KERNEL` 置位（例如已被 UART、I2C、SDMMC、regulator 驱动占用），Agent 与 UI 均禁止写入，并直接显示内核 `consumer` 名称。
2. **收藏与命名门禁**：全量引脚表默认只读浏览；用户需先将目标调试引脚加入“**常用控制引脚**”并赋予明确备注名（如 `4G_PWR_EN`、`HUB_RST`、`USER_LED`），首次对该引脚执行输出写入或脉冲时弹出确认框（提示引脚编号、备注名及硬件风险）。
3. **Rockchip 引脚名称换算器**：界面顶部内置 `GPIO<bank>_<group><index>` ↔ `<chip, offset>` / 全局编号互转工具（例如输入 `GPIO3_B4` 自动定位 `gpiochip3` 的 `offset 12`、全局编号 `108`），无需人工心算 `bank×32 + group×8 + index`。

---

## 5. iperf3 以太网测速工具方案（PC ↔ 板卡，全量随包分发）

### 5.1 测试拓扑与防误测规则

```text
┌──────────────────────────────────┐          局域网以太网直连 (TCP/UDP)          ┌──────────────────────────────────┐
│ Windows 上位机                    │ ─────────────────────────────────────────► │ 目标开发板 (Android / Linux)      │
│ 内置 third_party/iperf3/iperf3   │   客户端主动连接 -c <board_eth_ip> -p 5201   │ Agent 启动受控 iperf3 -s -1 -B   │
│ (-J 输出结构化 JSON 实时解析)     │ ◄───────────────────────────────────────── │ (随 Agent Bundle v2 自动部署)    │
└──────────────────────────────────┘          支持上行发送 / -R 下行接收          └──────────────────────────────────┘
```

- **PC 充当客户端、板卡充当一次性服务端（`--one-off`）**：
  - 无论测“PC → 板卡（上行）”还是“板卡 → PC（`-R` 下行）”，均由板端 Agent 在指定以太网 IP 上启动绑定地址的单次服务端 `iperf3 -s -1 -B <board_ip> -p <port>`，再由 Windows 上位机内置的 `iperf3.exe -c <board_ip> -p <port> -J` 主动发起连接。
  - **优势**：Windows 端始终是主动出站连接，**不会触发 Windows 防火墙入站拦截弹窗**；且板端服务端带 `-1`（跑完一次即退出）和 Agent 超时/取消守护，不会在板端遗留常驻监听进程。
- **严禁经过 ADB 转发**：
  - `Iperf3Service` 强制校验目标 IP 必须来自板端物理/虚拟网卡地址且不得为 `127.0.0.1` / `localhost`，防止误测成 USB ADB 转发速率。
- **同屏展示物理链路协商速率**：
  - 测速前与测速中自动调用只读能力 `network.interface.list` 读取板端网口（如 `eth0`）的 IP、MAC、`operstate`、协商速率（`/sys/class/net/eth0/speed`，如 `1000 Mb/s` 或 `100 Mb/s`）与双工模式（`duplex`）。当千兆网口仅协商为百兆时，界面直接给出醒目提示。

### 5.2 能力定义与参数硬边界

1. **`network.interface.list`（`read_only`，`agent_native`）**：
   - 只读枚举板端网络接口（排除或标记 `lo`），返回接口名、状态、IPv4 地址列表、速率 Mbps、双工模式与 RX/TX 字节及错包计数。
2. **`network.iperf3.server`（`network_load`，`runner_pack`）**：
   - 参数仅允许受控字段：`bind_ip`（必须匹配板端当前活动网卡 IPv4）、`port`（默认 `5201`，范围 `5201..5210`）、`timeout_sec`（范围 `5..120`）。
   - Agent 校验参数后启动随 Bundle 部署的固定路径 `iperf3` 二进制，不接受任何任意命令行参数拼接；任务取消或超时时终止子进程并确认端口释放。
3. **上位机 `Iperf3Service`（`src/application/device/`）**：
   - 先通过 Agent 启动 `network.iperf3.server` 并确认就绪，随后通过 `ProcessRunner` 调用本地内置 `iperf3.exe -c <bind_ip> -p <port> -t <duration> -P <parallel> [-R] [-u -b <bitrate>] -J --forceflush`；
   - 客户端参数硬边界：时长 `1..60 秒`（默认 `10 秒`），并发流 `1..8`（默认 `1`），协议 `TCP / UDP`（UDP 默认限速 `100M`、上限 `1000M`，防止无节制发包打死管理网口）；
   - 实时解析秒级吞吐（Mbps）、重传次数（Retransmits）或 UDP 抖动/丢包率，并在完成后汇总发送速率、接收速率、板端网卡协商速率与 CPU 占用，支持一键导出 JSON/文本报告。

### 5.3 打包与许可证清单

- **Windows 端**：在 `third_party/iperf3/` 纳入固定版本的 Windows x64 `iperf3.exe` 及配套运行时（如 `cygwin1.dll` 或 MinGW 编译产物）、`LICENSE`（BSD-3-Clause）与 `SHA256SUMS.txt`；MSBuild 构建时自动复制到 `build/windows/x64/<Configuration>/iperf3/`，并在 [`docs/THIRD_PARTY.md`](../THIRD_PARTY.md) 登记上游来源与摘要。
- **板端（3 个目标架构）**：使用上游固定版本 `iperf3`（3.x BSD-3-Clause）源码，通过现有 Android NDK（`arm64-v8a`、`armeabi-v7a`）与 Linux `aarch64-linux-gnu` 工具链构建自包含二进制，作为 `role=runner` 纳入 `src/agent/bundle/manifest.json`（Bundle v2 已预留多文件角色扩展位），部署时随 Agent 一并推送并校验 SHA-256。

---

## 6. 导航位置与分阶段实施路线

### 6.1 信息架构调整

```text
主导航
├── 串口调试
│   ├── 单串口调试
│   ├── 双端调试
│   └── 串口终端（P0 拆为独立页面组件；T1 升级为原生感网格终端；T2 升级为串口 + ADB Shell 多标签终端）
├── ADB 调试
│   ├── 远程调试
│   ├── 文件传输
│   └── 板级工具（G+N 新增：承载 GPIO 快速控制与 iperf3 网速测试）
└── 设备信息
```

### 6.2 分阶段实施与验收计划

| 阶段 | 范围 | Agent 版本 | 开发侧交付与验证 | 用户实机验证项 |
| --- | --- | --- | --- | --- |
| **阶段 P0：前置清理与架构解耦** | 清理根目录 `.obj`、旧 `build/iteration-*`、`src/agent/{obj,out,x64}`；移除未接入的 `board_profile.*` 与 `TestToolsAgent.vcxproj*`；从 `SerialWorkspace` 拆出独立 `SerialTerminalWorkspace`、`AtCommandDialog` 与 `SerialLinkVerifier`；按需启停 `scheduleTimer_`；加固 UI 测试 CMake 依赖 | 保持 `0.3.7` | 零外部行为变更；Debug/Release 构建、无 Qt 核心测试与全部 11 项原生回归测试 100% 通过；同步更新 `ARCHITECTURE.md`、`DEVELOPMENT.md` 与 `STATUS.md` | 主机与夹具结果只证明对应范围；实际串口收发和物理异常未验证，不能因重构目标是行为保持就宣称已通过 |
| **阶段 T1：原生感串口终端** | 引入 `libvterm`、实现 `TerminalGridWidget`、后台解析与 60Hz 脏区渲染、智能 `Ctrl+C`/`Ctrl+V`、选词/块选/折行还原、深浅配色 | 保持 `0.3.7` | 单元测试覆盖 VT 序列、备用屏幕、宽字符、折行复制；主机夹具压测 1.5Mbps 连续输出下帧耗时与 UI 响应；1366×768 / 1920×1080 快照 | Debug 串口启动日志、`vi`/`top` 全屏显示、复制粘贴手感 |
| **阶段 T2：内嵌 ADB Shell 与多标签** | 终端多标签栏、内置 ADB Shell 会话接入、窗口尺寸同步与外部终端回退 | 保持 `0.3.7` | 模拟 ADB 进程生命周期、多标签并发、设备断线隔离与窗口退出收敛测试 | 真实板卡内嵌 ADB Shell 交互、全屏命令与多标签切换 |
| **阶段 G+N：GPIO 与 iperf3 工具箱** | Agent 新增 `gpio.*` 与 `network.*` 能力；打包 Windows 与三端 `iperf3`；新增“板级工具”页（GPIO 面板 + iperf3 面板） | 递增至 `0.3.8` | 构建三目标 Agent 与 `iperf3`、更新 Bundle v2 清单与 SHA-256；无 Qt 契约测试与假 Agent 端到端测试；UI 快照 | Android 64/32 位与 Linux aarch64 板卡的 GPIO 输入/输出/脉冲/边沿监听，以及 PC↔板卡以太网上下行测速 |
| **阶段 U：UI 风格与布局精修** | 收敛 `theme.cpp` 色彩与间距令牌、按钮状态细节及终端/工具页视觉统一 | 不变 | 代表分辨率与高 DPI（125%/150%）快照对比与回归测试 | 整体视觉与日常工作流体验反馈 |

---

## 7. 不可妥协的工程边界检查

- `libvterm` 封装、双端验证协调器、GPIO 观察值模型与 iperf3 协调逻辑全部位于 `src/core/` 与 `src/application/`，**不引入任何 Qt 类型**。
- 板端 Agent 绝不开放任意命令或参数透传；GPIO 写入必须经过引脚占用检查与用户收藏确认，iperf3 服务端只接受受控绑定 IP、受限端口与超时。
- 所有新增第三方源码/二进制（`libvterm`、`iperf3`）均固定版本、记录 SHA-256 摘要与许可证，运行时不联网下载。
- 每完成一个阶段，同步更新对应 `docs/features/` 功能契约与 [`docs/STATUS.md`](../STATUS.md)，运行 `scripts/check-baseline.ps1` 并保持构建与测试通过。
