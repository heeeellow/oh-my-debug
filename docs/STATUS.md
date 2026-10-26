# TestTools 当前状态

[返回项目首页](../README.md)

更新日期：2026-10-26

本页是当前实现、验证、交付和开放风险的唯一汇总。行为规则见 [产品设计](PRODUCT.md) 和 `features/`；历史里程碑与旧版本验收见 [开发日志](DEVLOG.md)。

## 1. 当前基线与交付

- 分支：`master`。父基线为 `2d9ff3e`（`v0.8.0`），当前提交完成 **0.9.0**（独立串口终端、服务连接反馈、分层架构收敛与全仓 `src/` 中文注释统一）。
- 桌面版本由根目录 `VERSION.txt` 管理（`0.9.0`）；板端 Agent 保持 `0.3.7`，协议保持 `1`。本轮不改板端协议契约或部署包二进制。
- 本地版本标签已整理为 `v0.1.0`、`v0.3.1`、`v0.4.2`、`v0.8.0`、`v0.9.0`；当前本地提交与新增标签尚未推送至远端（远端 `origin/master` 仍为 `a494dc9`，本地领先 6 个提交）。
- 主入口：`TestTools.slnx`；产物：`build/windows/x64/<Configuration>/TestTools.exe`。`0.9.0` 验收证据目录：`build/iteration-0.9.0/`。

## 2. 当前实现

### 串口与终端

- 单/双串口调试保留 Windows COM、设备 Agent 串口、文本/HEX、快捷指令、共享普通行尾、独立定时/次数发送及双端验证/可靠性压测。规则见 [串口契约](features/SERIAL.md)。
- 串口终端为独立页面和会话，使用公共连接栏与接收组件，各自保存参数、计数、发送队列和显示缓冲；切页保留连接，终端 RX 不进入普通监视器。
- 主机端口登记按 Windows COM 别名和设备序列号检查占用；同口提示占用页面，异步关闭完成后释放。每台 Agent 仍只有一个活动串口会话；OS/Agent 保留最终裁决。
- 终端偏好保存于 `terminal-preferences.json`，首次从旧通道 B 读取一次；配色不改。进入终端页与连接成功后恢复输入焦点。
- 连续 UTF-8/ANSI、直接输入、设备历史/补全/中断、复制/粘贴、搜索和导出继续使用 [终端契约](features/SERIAL_TERMINAL.md)。回看上限为 10000 行/1M 字符单元；淘汰旧行采用增量文档更新。
- 本地串口保留 RX 事件立即通知；本轮未改 CSerialPort 适配器或其驱动参数。偶发不回显仍需按发送、接收、解析、显示证据判断。

### 设备、文件与投屏

- 顶栏将“设备串口服务”状态与连接/准备、详情、重试按钮分开；按钮有悬停/按下/禁用反馈，准备期间展示实际步骤。ADB、Windows 本地 COM 与服务状态分别维护。
- 本地 COM 活动不阻止服务准备、断开和重连；设备串口活动保留维护保护。服务健康检查继续每 3 秒以独立连接校验握手、版本和串口能力；旧代次结果失效。
- Android/Linux ADB 发现、按需准备 Agent、版本/能力/系统信息检查保持；固定 Bundle v2 继续包含 Android 两 ABI、Linux aarch64/glibc-2.31 三个目标。
- ADB 文件页保留双栏浏览、原生异步图标、筛选/排序、拖放、固定设备批次、手动收发、暂存替换、逐项结果、取消与失败重试。自动进入/完成刷新使用已打开目录，不提交本地或设备地址草稿。规则见 [文件契约](features/FILE_TRANSFER.md)。
- 远程调试保留独立系统 ADB Shell 与 scrcpy 4.1 投屏，均不要求 Agent。投屏固定会话设备、显式启动、独立画面与 `--no-audio`；规则见 [投屏契约](features/SCREEN_MIRROR.md)。
- 左侧串口/ADB 二级导航保留父子选中、折叠与上次子页；启动进入单串口，设备信息位于底部。

## 3. 本轮验证

各层结果分别记录，未验证项不能由其他层通过替代。

| 范围 | 结果与边界 |
| --- | --- |
| 修改前 COM3 基线 | CH343、115200/8N1/无流控。原始串口与旧产品 UI 能回到 Shell、执行 printf、Ctrl+C 中断短暂 sleep；旧 UI TX 48/RX 452 字节。本次没有复现用户报告的偶发不回显。证据为 `baseline-com3-*`、`baseline-ui.*` |
| Agent 当前实机 | RK3562 / Android 13，进程存在且真实 hello 返回 0.3.7；只读转发与握手通过，已清理本次临时转发。用户另已反馈该版部署验证通过，属于用户提供证据 |
| 核心契约 | CTest 2/2 通过；新增同口登记/释放、COM 别名、按设备单会话和终端截断增量变化范围检查。记录位于 `build/tests/host/Testing/Temporary/LastTest.log` |
| 原生界面与异常夹具 | CTest 5/5 通过：文件页、串口交互、文件后端、服务健康/失联、独立页面。覆盖偏好迁移、同口拒绝/释放、本地地址草稿、离线本地刷新及设备端口枚举不阻塞本地连接。10000 行缓冲加 30 次尾部更新的独立样本最大交付 27ms；这是本机夹具样本，不是性能保证。记录为 `native-tests/Testing/Temporary/LastTest.log`、`fixture-ui.stdout.log` |
| 桌面构建与 Bundle | Release/Debug 最终构建通过；Bundle v2 三包文件、大小和 SHA-256 校验通过。Qt 部署提示缺少可选的 dxcompiler/dxil DLL，当前 Widgets 启动与快照正常。记录为 `build-release.log`、`build-debug.log` |
| 真实 Debug 串口 | COM3、115200/8N1。逐字输入回显、命令执行、↑ 历史、Tab 补全、Ctrl+C 中断通过；中断样本 61ms。10500 行有界合成输出在 9.130s 收完，超过回看上限后仍能输入；页面独立、同口占用、断开后普通监视器接管及真实 RX 标记通过。记录为 `live-ui.stdout.log` 和 `live-*-rx.bin`；后者仅为保留片段，不是全程原始归档 |
| 真实服务与 ADB | 本地 COM 保持打开时，Agent 连接、断开、重连和设备端口枚举通过。Agent PID 保持 1789；最终无新增 ADB 转发残留，见 `live-cleanup.json`。文件页只读进入根目录、浏览 `/sdcard`、切页刷新通过（16 项目录样本），见 `live-files.stdout.log`；未执行真实 push/pull |
| 快照与运行依赖 | 单/双串口、终端、文件页，在 1366×768 / 1920×1080 逻辑尺寸及实际 DPR 1 / 1.25 / 1.5 共 24 张快照的尺寸元数据通过，并目检代表样本与实机界面。使用 Release 部署目录且移除开发 Qt PATH 后启动通过。见 `snapshot-matrix.json`、`snapshots/`、`terminal-live-*`、`service-live-1366.png` |
| 文档与静态边界 | `check-project-docs.ps1`、`check-baseline.ps1` 与 `git diff --check` 通过；未暂存任何文件，新增模块与项目登记已核对 |
| 0.9.0 后架构收敛与注释整理 | 在不改变 UI 样式与业务行为的前提下：1）`agent_bundle.cpp` 与 `agent_serial_transport.cpp` 统一复用 `core/protocol/json_value.*`，移除手写重复 JSON 解析器与正则提取；2）`agent_service_status.*` 新增 `parseAgentServiceSnapshot`，移除 `MainWindow` 对原始协议 JSON 的直接解析；3）`serial_test_engine.*` 下沉 `minimumStressIntervalMs`、`appendAndMatchFrame` 与 `evaluateStressDirection`，减轻 `SerialWorkspace` 算术与协议匹配负担；4）`ui/theme.h` 统一提供 `fromUtf8` / `toUtf8`；5）`src/` 全部源码英文注释统一替换为中文并补充关键模块说明。验证：Debug/Release 构建通过，主机无 Qt CTest 2/2 与 Agent CTest 1/1 通过，原生 UI CTest 5/5 通过；本轮重构的实机串口/ADB/板端与长时间压力 **未验证** |

0.8.0 与更早版本的界面、文件实机和板端验证已归档到 [开发日志](DEVLOG.md)。这些历史结果不自动证明 0.9.0 通过。

## 4. 开放风险与后续

- 本轮目标是启动日志与 Shell 输入/回显/历史/补全/中断；完整 TUI、备用屏幕、滚动区域和完整宽字符表不在当前兼容范围，不能据此宣称替代 vim/top 终端。
- 串口桥接的阻塞发送、短写、关闭边界、完整原始记录以及 GUI 接收队列上限仍需按具体场景继续收敛；界面回看缓冲不等于完整串口日志归档。
- 文件提交阶段取消与替换可能竞争，断线可能遗留本批临时目录；正在变化的日志不保证一致性快照。目录合并仍未实现。
- 偶发“不回显、回车也无输出”在修改前后均未复现，尚未确定根因，不能宣称彻底解决；需继续保留后续复现与分段取证入口。
- 本轮物理拔线、重启全程启动日志、Agent 板端串口实际收发、长时间高波特率/大文件压力、真实 push/pull、投屏音频共存、实际中文 IME **未验证**。合成日志、回环协议和假后端不能替代这些结果。
- `MainWindow` 仍拥有连接界面状态/异步代次，`SerialWorkspace` 仍协调双端验证和压测的 UI 调度（底层服务快照解析、线速间隔估算、滑窗令牌匹配与单向压测统计判定已下沉至 application/core）；更大规模的视图拆分、命名空间统一和构建体系迁移后置。
