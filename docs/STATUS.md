# 当前状态

[README](../README.md) · [产品说明](PRODUCT.md) · [开发手册](DEVELOPMENT.md)

## 版本与发布

更新日期：2026-11-06。桌面版本 **0.18.1**，Agent **0.3.12**，协议 **1**。

- 正式版本：[v0.18.1](https://github.com/heeeellow/oh-my-debug/releases/tag/v0.18.1)。默认分支为 `master`。
- 提供 Windows 10 1809 及以上、Windows 11 x64 的完整目录便携 ZIP，包含 Qt、MSVC 运行库、ADB、scrcpy、iperf3 与 Agent。
- 公司版权署名为陕西瑞迅电子信息技术有限公司，开发者署名为 Yanzhaohui。
- 第三方许可与 Cygwin 对应源码随包提供，版本和来源见 [第三方说明](THIRD_PARTY.md)。
- 源码以发布标签为准，附件记录构建输入及文件摘要。单文件 EXE 和安装器尚未实现。

## 0.18.1 主要变化

GPIO 使用一份物理引脚列表，关联设备名称、Linux 号、占用状态和备注。
后台按已确认能力选择字符设备或驱动接口，多映射不明确时禁止操作。
限时保持与持续设置分别说明，设置输出会持续生效，不自动恢复。

网络测速重新排列两端地址、方向、协议和时长，高级数字项支持键盘、手输及上下箭头。
Loader 入口始终显示，后台只读判断接口支持情况；请求前再次检查，执行需要确认。
ADB 离线只能说明结果待确认，不能视为已进入 Loader。

操作规则见 [工具箱契约](features/TOOLBOX.md) 和 [维护契约](features/DEVICE_MAINTENANCE.md)，
设计原因见 [ADR-0009](decisions/0009-unified-gpio-controls-and-maintenance-plan.md)。

## 已有验证

以下记录对应 0.18.1 的构建与发布检查，不能代替后续源码改动后的验证。

| 项目 | 结果 |
| --- | --- |
| 桌面构建 | Release、Debug 通过；MSVC 14.44、Qt 6.11.2，隔离构建目录 |
| 核心与界面 | 核心 8/8、原生 UI 15/15；2026-11-06 复跑既有 Agent 检查 1/1 |
| 分辨率与 DPI | 1366×768、1920×1080、最小 1120×720；100%、125%、150% 检查通过 |
| 公司署名成品 | Release 构建及 EXE 公司、版权、开发者和版本属性核对通过 |
| 便携包 | 公司包 268 项载荷逐项核对；清除 Qt 环境变量、限制 PATH 后离线启动及配套工具版本检查通过 |
| 发布附件 | 公开下载、大小和 SHA-256 已核对；源码标签与附件版本对应 |
| 文档与静态检查 | 文档入口、相对链接、分层和 VS 导航检查通过 |

核心与 Agent 的复跑使用既有检查程序，不是最终公司版全部构建对象的重新测试，
也未覆盖下述已知问题的故障注入。

界面与逻辑验证见 [0.18.1 验证记录](reviews/ui-refinement-0.18.1-validation.md)。
本地证据目录为 `build/ui-refinement-0.18.1/` 和 `build/public-release-0.18.1/`。

## 实机验证范围

- MTB-720（RK3568）：11 个已适配 GPIO 端点只读检查通过；gpio1 对应 GPIO3_B5 / Linux 109。
- RTD-XL01（RK3562）：otg_drv_vbus 只读检查通过，对应 GPIO2_A0 / Linux 64。
- 两板已识别 Rockchip 兼容项、syscon-reboot-mode 与 Loader 模式值 `0x5242c301`；这不代表实际切换成功。
- 0.18.1 只读检查没有执行 GPIO 输出、脉冲、网络测速、Root/Remount、重启或 Loader 请求。
- [0.17.1 网络实测](reviews/toolbox-0.17.1-feedback-validation.md)记录 TCP 接收约 949/947 Mbps、UDP 20 Mbps 双向零丢包；0.18.1 改版后未重复该实测。

## 已知问题

- 同一设备使用多个实例时，转发清理可能影响其他实例；建议每台设备只由一个实例操作。
- 普通 ADB 命令超时的子进程处理可能影响共享进程。
- GPIO 监听终态前积压较多事件时，报告可能未取完最后的事件。
- Agent 尚缺控制连接数量与空闲期限限制。

这些问题来自静态代码审查，尚未进行实机故障注入复现。

## 未验证项目

- Loader 实际切换、USB 模式确认及退出恢复。
- GPIO 输出、电压、负载、物理边沿、断线、休眠、外部并发和长时状态。
- 0.18.1 改版后的真实以太网测速，以及更多驱动、SELinux、权限和固件组合。
- 全新 Windows 环境、长时压力，以及公司署名构建后的完整异常回归。

后续设计见 [原生终端与工具箱](decisions/0007-native-terminal-gpio-and-iperf3-plan.md)。
历史验证见 [0.18.0](reviews/board-gpio-0.18.0-validation.md) 和
[0.17.1](reviews/toolbox-0.17.1-feedback-validation.md)。
