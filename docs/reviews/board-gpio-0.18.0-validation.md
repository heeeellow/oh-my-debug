# 公司 GPIO 接口兼容验证记录

[当前状态](../STATUS.md) · [工具箱契约](../features/TOOLBOX.md) · [方案与 BSP 依据](../decisions/0008-gpio-ownership-and-board-adapters-plan.md)

日期：2026-11-05。此记录说明本轮证据和验证边界，完成度、版本和交付结果统一见 STATUS。
本地证据根目录：`build/gpio-board-0.18.0/`。没有为本项目写入共享知识库。

## 源码与实现范围

只读读取用户指定 A11 内核的 `drivers/mfd/rk-gpio.c`，并定位同目录 `rock-gpios.c`。
源码 revision、大小和 SHA-256 保存在 `evidence/bsp-source-context.json`，核对副本只留本地忽略目录。
服务器源码和内核均未修改、构建或部署。

两个 ABI 的底层命令不是普通的 0/1 写入：写 0 切输入，写 10/11 才输出低/高。
适配将 UI 的物理电平参数转换为固定命令，读取不会设置方向。BSP store 没有透传 GPIO 配置错误，
因此软件需要区分提交字节、读回和方向复核，不能把写函数返回成功直接算成外部电平已验证。

本次新增 Agent 的 `BoardGpioRegistry` 与固定能力，桌面复用原工具箱任务/确认/报告。
原有字符设备占用保护保留，公司接口保持驱动占用。未实现解绑、unexport、模块卸载或寄存器操作。

## 软件证据

| 范围 | 本地证据 | 验证说明 |
| --- | --- | --- |
| Windows Release / Debug | `release-build.log`、`debug-build.log` | 显式指定 MSVC 14.44.35207；独立输出和对象目录，保留旧运行程序 |
| Windows 核心 / Agent | `core-ctest.log`、`agent-ctest.log` | 固定端点参数、不接受路径/底层命令、任务票据/租约/重复操作等；不访问硬件 |
| Linux 主机文件系统夹具 | `linux-build.log`、`linux-ctest.log` | 临时目录模拟两类 show/store，测试命令 10/11、物理映射、占用者改变、节点替换/符号链接、取消、部分写、写后读回失败和持续设置 |
| 原生界面 | `ui-ctest.log`、`ui-evidence/` | 全部/通用/板载来源、合并行、初始化偏好加载、跨来源定位、已占用可读、确认取消/设置、只读用途及旧 Agent |
| 三目标 Agent | `agent-matrix.log`、`agent-bundle/manifest.json` | Android ARMv7 / ARM64、Linux aarch64/glibc-2.31；逐文件大小和摘要校验 |
| 分辨率与 DPI | `snapshots/`、`ui-evidence/`、`live-evidence/` | 完整窗口与板载状态分开抓图，1366×768、1920×1080、最小宽度及高 DPI；实际尺寸按快照记录核对 |

主机模拟中的 store 回调只在测试根目录模拟内核属性，产品没有可通过 RPC 替换根目录或写函数的入口。
模拟成功不证明真实驱动、电压、负载或外部连线合格。

## 实机只读证据

| 主板 | 发现与读取 | 设置入口 |
| --- | --- | --- |
| MTB-720 / `66c8c84745c4330`，4.19.193+ | 11 个公司端点逐个执行 `gpio.board.read`；设备树与原生行信息关联真实管脚 | gpio1…gpio10 的元数据允许设置；sim_det 只读。实际输出未验证 |
| RTD-XL01 / `ac7f17a16344038`，5.10.157+ | otg_drv_vbus 已映射到 GPIO2_A0 / Linux 64，执行读取 | VBUS 用途只读，未发送设置 |

每台设备的 `live-evidence/<serial>/readonly-result.json` 保留实例、端点清单和所有读取结果。
`live-board-read.json` 为 UI 实际发起的公司接口读取报告；入口断言 capability 为 `gpio.board.read`，
并等待来源筛选和结果完成渲染后抓图，避免把通用 GPIO 页面误记为板载接口验收。

`before.txt` / `after.txt` 比较所有已申请 GPIO 的占用者/方向及公司节点的数字值。
MTB-720 的另一个 GPIO 133 / work LED 采用内核 heartbeat，完整快照中的该电平可能自然变化；
初次对完整文本不变的断言因此过严，前后证据保留在 `evidence/led-transition/`。
后续比较不把无关闪灯当成工具写入，也不从一次前后电平相同推断外部电气行为。

只在各板的独立临时目录和未占用端口启动测试 Agent，没有替换日常服务。
`live-cleanup.json` 核对自己的 PID/exe 后停止并移除自己的转发。实际 GPIO 写入计数为零，没有 Root/Remount 或重启。

## 未验证与交付边界

- 公司接口真实输出、方向切换后的物理电压/负载，以及设置后的断线、休眠、Agent 被杀与外部并发写入：未验证。
- rk_gpio 的 platform 命名属性布局：源码和 Linux 模拟验证；真实对应固件未验证。
- 其他目录、同名 compatible 但修改 ABI 的固件、更多命名用途、权限/SELinux 组合和长时压力：未验证。
- 既有串口、网络等提供软件回归，本轮未新增 COM 通信、实际网速流量或整机维护验证。
- 未生成 ZIP、未提交、打标签或推送。最终发布前的干净 Windows 安装和第三方对应源码/许可核对仍按原安排处理。
