# Cygwin 3.6.5-1 对应源码

随包 `cygwin1.dll` 来自固定的 ar51an iperf3 3.20 Windows 归档，未作修改。
版本输出及上游版本表均为 Cygwin 3.6.5-1；DLL、源码包及修订摘要以 `CYGWIN-SOURCE.json` 为准。

Windows 发布包在 `licenses/iperf3/` 中同时携带
`Cygwin-3.6.5-1-corresponding-source.zip`，内含：

- 官方 `newlib-cygwin` 仓库的 `cygwin-3.6.5` 标签完整导出，提交 `2ae6825d022ac4450cef168ce066d68810b3a6e2`。
- 官方 `cygwin-packages/cygwin` 仓库提交 `e68f921332ce8fe9b265bc0f620356e0de7f7b01` 的完整导出，包含 `cygwin.cygport`。
- 来源、文件摘要、随包 DLL 摘要、许可措辞核对及构建说明。

该构建配方设置 `VERSION=3.6.5`、`RELEASE=1`、
`GIT_REV=cygwin-3.6.5-0-g2ae6825d022a`，列出构建依赖、编译参数与安装步骤，没有另行声明源码补丁。
源码包由上述官方 Git 修订导出，不冒充已经退役的历史镜像发行包。
现有三份许可文本与对应源码的许可措辞逐词核对一致，保留原有排版；未重编译 DLL，也不声称二进制可复现。

更换 Windows 运行时后，应重新核对实际版本、对应源码与构建配方，并更新清单，不能只沿用本文。
第一方公司许可不限制 Cygwin、newlib 和其他上游文件各自的许可证与权利。

来源：

- [Cygwin 官方源码](https://cygwin.com/cgit/newlib-cygwin/tag/?h=cygwin-3.6.5)
- [官方 3.6.5-1 构建配方](https://cygwin.com/cgit/cygwin-packages/cygwin/tree/cygwin.cygport?id=e68f921332ce8fe9b265bc0f620356e0de7f7b01)
- [官方发行公告](https://cygwin.com/pipermail/cygwin-announce/2025-October/012543.html)
- [固定 Windows 分发版本](https://github.com/ar51an/iperf3-win-builds/releases/tag/3.20)
- [Cygwin 许可说明](https://cygwin.com/licensing.html)
