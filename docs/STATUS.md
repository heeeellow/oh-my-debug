# 当前状态

[README](../README.md) · [产品说明](PRODUCT.md) · [开发与验证](DEVELOPMENT.md)

本页是当前实现、验证范围和交付状态的唯一入口；历史验证不能替代本轮检查。

## 1. 版本与交付

- 日期：2026-11-06。桌面 **0.18.1 / Oh My Debug**，Agent **0.3.12**，协议 **1**。
- 源码归档分支为 `master`，远程目标 `origin/master`；本版包含自 `d01ac1e` 起的工具箱与维护改动，更新内容见 [0.18.1 更新说明](releases/0.18.1.md)。
- 仓库已公开，2026-11-06 发布首个正式 [GitHub Release v0.18.1](https://github.com/heeeellow/oh-my-debug/releases/tag/v0.18.1)，标记为 Latest，非草稿、非预发布。
- 发布标签 `v0.18.1` 固定到 `5265dab568dfec92496f7e9e4bcd98faf9d10b12`，与公司版 ZIP 的源码清单一致。
- 公司署名程序：`build/public-release-0.18.1/runtime/Release/OhMyDebug.exe`；本次单独构建 Release。前轮 Debug 位于 `build/ui-refinement-0.18.1/runtime/Debug/`，未重新构建。
- 本次将第一方版权署名统一为陕西瑞迅电子信息技术有限公司，保留 Yanzhaohui 的开发者署名，并补齐 Cygwin 对应源码。未修改产品业务逻辑；沿用已校验的三目标 Agent 0.3.12 Bundle，未重新交叉编译或操作日常设备服务。
- 公司署名正式便携包放在 `build/packages/0.18.1-company/OhMyDebug-0.18.1-windows-x64.zip`，旁边提供 `.sha256`；两份文件均已上传到上述 Release。源码归档前的预览证据在 `build/public-release-0.18.1/preview/`。旧个人署名包 `build/packages/0.18.1-local/` 保留，不能作为公司署名成品发布。
- ZIP 面向 Windows 10（1809 起）/11 x64，随包部署 Qt、MSVC app-local 运行库、ADB、scrcpy、iperf3 与 Agent，无需测试人员安装开发环境或配置 PATH。
- 旧 0.16.0 ZIP 保持原样。实际硬件仍需 Windows 能识别其 COM 或 ADB 设备，本包不安装设备驱动。

## 2. 本轮实现

- GPIO 只有一份引脚列表，按物理行关联设备名称与用户备注，去掉来源选择和私有化文案；保留真实方向、占用者和 Linux 号。
- 后台按已确认接口和已公布能力组织读取、定时输出或持续设置；多映射不确定时禁止操作。常用收藏可选，旧备注并存保留。
- 保持输出明确到期释放；设置输出明确切输出、持续生效、不自动恢复。没有新增解绑、任意路径或未知命令写入。
- 网络参数重新分组：地址各占一行，方向/协议/时长同一行，紧凑时长框位于右侧并对齐地址右边缘；高级数字项紧凑排列，Tab 顺序跟随布局。
- 共用数字框补齐上下箭头，保留手输、键盘、单位与原有范围。
- 维护动作去掉统一省略号，导出文件保留省略号。Loader 始终可见，后台只读识别当前设备的重启模式接口，缺失、未知、冲突等分别显示原因。
- Loader 支持判定按设备上下文与期限失效，请求前再次核验；固定请求仍需一次默认取消的确认。ADB 离线只记为结果待确认。

行为依据：[工具箱契约](features/TOOLBOX.md)、[设备维护契约](features/DEVICE_MAINTENANCE.md)。
设计依据：[ADR-0009](decisions/0009-unified-gpio-controls-and-maintenance-plan.md)。

## 3. 本轮验证

证据目录：`build/ui-refinement-0.18.1/`，详细记录见[本轮验证记录](reviews/ui-refinement-0.18.1-validation.md)。

| 类型 | 结果与边界 |
| --- | --- |
| Release / Debug | 均构建通过；MSVC 14.44，独立对象与运行目录，Bundle 摘要校验通过 |
| 无 Qt 逻辑 | 8/8，通过统一资源、旧备注、占用/能力限制，以及 Loader 证据解析、跨设备旧结果与取消等检查 |
| 原生 UI | 整套 15/15 通过；最后的高级入口位置调整后，用最终对象补跑工具箱通过 |
| 分辨率与 DPI | 1366×768、1920×1080、最小 1120×720 快照已查看；100%/125%/150% 数字框操作、参数对齐、键盘与禁用态通过 |
| 静态与文档 | 文档链接、分层、VS 导航、Git 空白与本轮差异范围检查通过 |
| Agent / 跨平台构建 | 本轮未修改 Agent；未重跑 Android/Linux 交叉编译或 Linux 文件系统夹具，前轮证据保留在历史记录 |
| ZIP 便携包 | 265 项载荷摘要与解压文件一致；40 个 Windows PE 文件的普通/延迟导入检查通过；清除 SDK/PATH 后三页离线启动通过，Qt/MSVC 实际从包内加载，三个配套工具版本启动通过 |
| 干净系统 / 长时 | 本机不具备 Windows Sandbox；未验证全新 Windows 测试机和长时压力，不能把移除 PATH 的本机检查等同于纯净系统 |

### 2026-11-06 公司署名与分发补验

证据目录：`build/public-release-0.18.1/`。本次没有改变界面布局或设备业务逻辑。

- Release 独立构建通过，使用 MSVC 14.44；核对 EXE 的版本、公司、版权和开发者属性，中文公司全称显示正确。
- 打包脚本拒绝旧个人署名 EXE 与摘要不符的 Cygwin 源码包，均在创建载荷目录之前失败。
- 公司预览包 268 项载荷逐项核对大小和 SHA-256，零差异；其中包含固定 Cygwin 官方源码、3.6.5-1 构建配方和来源清单。
- 解压后移除 Qt/QML 环境变量、将 PATH 限于 Windows 目录，从独立工作目录离线启动串口终端通过；1366×768 逻辑尺寸快照已查看。三个配套工具的版本启动通过。
- 本日架构审查复跑既有检查程序：核心 8/8、Agent 1/1、原生 UI 15/15 通过；它们不代表对下面已知问题的故障注入验证，也不作为新构建对象的全套重测。
- 静态分层、VS 导航、依赖摘要、Git 空白和文档链接检查通过。新 Debug、Agent 矩阵、干净 Windows、真实设备动作、异常和长时压力：本次未验证。
- 正式发布后通过 GitHub API 核对标签提交、非草稿/非预发布状态和两份附件的大小、SHA-256，均与本地一致；ZIP 为 55,760,699 字节。未登录的公开校验文件下载与本地一致。发布核对记录为 `github-release-verification.json`，页面凭据为 `github-release-v0.18.1.jpg`。

## 4. 实机只读范围

- MTB-720（RK3568）11 个已适配端点逐个读取通过；统一 UI 自动通过对应接口读取，gpio1 对应 GPIO3_B5 / Linux 109。
- RTD-XL01（RK3562）otg_drv_vbus 读取通过，对应 GPIO2_A0 / Linux 64，保持用途只读。
- 两板节点值、GPIO 占用者和方向前后一致；MTB work LED 的无关 heartbeat 电平变化单独记录。
- 两板均用实际桌面后端识别当前运行固件：Rockchip 兼容项、已绑定 syscon-reboot-mode、mode-loader `0x5242c301`。这确认请求接口，不证明实际进入 Loader。
- 本轮没有发送 GPIO 输出/脉冲、网络测速流量、Root/Remount、普通重启或 Loader 请求。仅停止本轮临时进程与转发，日常服务保持。

## 5. 未验证与后续

- 真实进入 Loader、USB 模式确认、退出与恢复：未验证，需设备具备可恢复条件时另行安排。
- GPIO 真实输出、电压/负载、物理边沿、断线/休眠/外部并发和长时状态：本轮未验证。
- rk_gpio 属性布局与更多驱动、SELinux/权限组合：本轮未做新增固件的实机验证。
- 旧个人署名试用包证据在 `build/zip-trial-0.18.1/`。公司包证据在上节目录；Cygwin 对应源码已经补齐，其他验证不能据此自动视为通过。
- 同设备多实例转发清理、普通 ADB 超时子进程处理、GPIO 终态事件分页取尽，以及 Agent 连接限额/空闲期限仍待改进；本次未修改这些代码或实机复现，发布说明保留已知问题。
- 内嵌终端 T2、整体视觉精修和多板并发仍后置，见[原迭代方案](decisions/0007-native-terminal-gpio-and-iperf3-plan.md)。

历史：[0.18.0 适配验收](reviews/board-gpio-0.18.0-validation.md)、[0.17.1 反馈验收](reviews/toolbox-0.17.1-feedback-validation.md)。
