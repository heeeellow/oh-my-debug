# ADR-0005 ADB 工作区与文件传输

[当前状态](../STATUS.md) · [产品设计](../PRODUCT.md) · [界面设计](../UI_DESIGN.md)

日期：2026-10-21。状态：已采用。

## 背景

串口终端、ADB Shell、文件传输和 Android 投屏需要共用设备选择，同时保留各自的会话与任务。
文件传输应能独立完成日志下载和文件上传，不依赖内置终端或远程编辑器。

## 设计决定

- 界面继续使用 Qt Widgets。串口和 ADB 功能采用左侧二级导航；父级文字恢复上次子页，箭头只展开或收起。
- 顶栏负责设备选择和连接状态。Shell、文件和投屏操作放在对应页面，页面切换保留已有任务。
- 文件页采用本地与设备双栏，本地初始目录为桌面。使用者选择源项目和目标目录后手动上传或下载。
- 文件页独立维护设备目录。外部 Shell 中执行 `cd` 不会同步文件页；可以执行 `pwd` 后复制路径。
- 普通文件同名时默认跳过，覆盖需要明确选择；已有目录不合并。文件夹传输保留名称和层级。
- 支持目录链接浏览，但递归传输不自动跟随链接。失效链接、权限不足和设备离线分别报告。
- 本地编辑后重新选择文件上传，不维护待写回状态、编辑器监听或自动上传。
- ADB Shell 保留独立终端窗口；投屏使用 scrcpy 独立画面，控制与截图固定在启动设备上。

现行导航和操作细节见 [界面设计](../UI_DESIGN.md)、[文件传输契约](../features/FILE_TRANSFER.md)、
[ADB 终端契约](../features/ADB_TERMINAL.md) 和 [投屏契约](../features/SCREEN_MIRROR.md)。

## 原因与影响

双栏浏览让源项目、目标目录和冲突处理同时可见。文件传输不依赖 Shell 会话，可单独处理进度、
取消和失败重试。设备切换不会改变已开始任务的目标，避免向另一台设备继续写入。

Qt 模型、窗格和图标缓存放在 UI 层；目录读取、传输与进程操作在后台执行。
文件列表绘制不查询设备或同步读取 Windows Shell 图标。

ADB 的权限按实际命令判断。外部 Shell 中执行 `su` 不会使独立 `adb push/pull` 自动获得 root；
Root、Remount 和重启使用独立维护动作，传输失败不自动追加提权或修改安全策略。

## 投屏与音频

投屏采用纯画面模式，不转发音频。音频播放、录音与 AEC 共存需要在目标设备上分别验证。
对 scrcpy 与其他工具的体验差异，没有完成相同设备、版本和音频路径的对照前，不作原因判断。

## 后续方向

内嵌 ADB Shell、自动目录同步、目录合并和投屏画面嵌入需要独立设计与验证。
原生串口终端的后续选型记录在 [ADR-0007](0007-native-terminal-gpio-and-iperf3-plan.md)。
这些功能不作为现有手动文件传输的使用前提。

## 参考

- [ADB 命令](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/user/adb.1.md)
- [Qt 模型与视图](https://doc.qt.io/qt-6/model-view-programming.html)
- [scrcpy 4.1 音频说明](https://github.com/Genymobile/scrcpy/blob/v4.1/doc/audio.md)
