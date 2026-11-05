# 当前状态

[README](../README.md) · [产品说明](PRODUCT.md) · [开发与验证](DEVELOPMENT.md)

本页是当前实现、验证范围和交付状态的唯一入口；历史验证不能替代本轮检查。

## 1. 版本与交付

- 日期：2026-11-05。桌面 **0.18.1 / Oh My Debug**，Agent **0.3.12**，协议 **1**。
- 源码归档分支为 `master`，远程目标 `origin/master`；本版包含自 `d01ac1e` 起的工具箱与维护改动，更新内容见 [0.18.1 更新说明](releases/0.18.1.md)。
- 当前处于发布准备：已制作本地 ZIP 便携试用包；尚未创建正式发布标签或 Release。源码同步和本地试用不等于公开发布。
- 新程序：`build/ui-refinement-0.18.1/runtime/Release/OhMyDebug.exe`；Debug 在同级目录。
- 本轮只改桌面与文档，沿用 `build/agent-bundle/` 的三目标 0.3.12 Bundle，已复制到独立运行目录；未重编译或替换日常设备服务。
- 用户已选择先试用 ZIP：`build/packages/0.18.1-local/OhMyDebug-0.18.1-windows-x64.zip`，旁边提供 `.sha256` 校验文件；单文件便携 EXE 尚未实现。
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
- 试用包证据在 `build/zip-trial-0.18.1/`：package-check.json、pe-imports.json、tools-check.json、startup-result.json 及截图。最终正式发布前仍需干净 Windows 验收和第三方对应源码/许可核对，不将本地试用等同于发布。
- 内嵌终端 T2、整体视觉精修和多板并发仍后置，见[原迭代方案](decisions/0007-native-terminal-gpio-and-iperf3-plan.md)。

历史：[0.18.0 适配验收](reviews/board-gpio-0.18.0-validation.md)、[0.17.1 反馈验收](reviews/toolbox-0.17.1-feedback-validation.md)。
