# 第三方依赖

[返回项目首页](../README.md)

当前依赖以 vendor 源码方式纳入主仓库，保证首次克隆即可构建。升级依赖时必须记录新的上游提交、
重新构建并运行自测，不直接覆盖本地适配。

| 依赖 | 上游 | 基线提交 | 本地适配 |
| --- | --- | --- | --- |
| CSerialPort | <https://github.com/itas109/CSerialPort.git> | `cb8c8b4bb358c80a01f0605367a30b07643863cb` | 无 |
| standalone Asio | <https://github.com/chriskohlhoff/asio.git> | `8806a6803cde7054c3049d3666d3ec36786568c5` | 无 |
| tiny-process-library | <https://github.com/eidheim/tiny-process-library.git> | `4c4f1c68e90423e0b43091deeebcdb6c0fc02fb2` | `process_win.cpp` 为 Qt GUI 后台命令补齐有效 stdin，并使用 `CREATE_NO_WINDOW` |

每个依赖的许可证文件保留在其源码目录中。原嵌套仓库的 Git 元数据仅在当前开发机上备份到主仓库
`.git/vendor-repo-metadata/`，不属于提交内容。
