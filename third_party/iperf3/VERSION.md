# iperf3 固定依赖

- 上游：ESnet iperf3 3.20；[发布说明](https://github.com/esnet/iperf/releases/tag/3.20)。
- 原始源码包：[iperf-3.20.tar.gz](https://downloads.es.net/pub/iperf/iperf-3.20.tar.gz)，SHA-256 `3acc572d1ecca4e0b20359c7bf0132ddc80d982efeee20c86f6726a9a6094388`。
- 本地仅增加 CMake 入口与平台特征探测配置；上游源码压缩包不改写，构建在输出目录解压。关闭额外认证依赖，保留 TCP/UDP、JSON 流、线程与受控服务参数。
- Android 使用既有 NDK、API 23，Linux aarch64 使用仓库的 glibc 2.31 工具链；目标执行文件进入 Agent Bundle，清单逐文件校验。
- Windows x64：[ar51an 3.20 固定发布](https://github.com/ar51an/iperf3-win-builds/releases/tag/3.20)的 `iperf-3.20-win64.zip`，SHA-256 `fc2a736b35559d410b7032e2be3de45a6884d5dcf9d12b96e759b1e2e7076b7b`；只取 `iperf3.exe` 与 `cygwin1.dll`。
- 该 Windows 产物的版本输出为 iperf 3.20 / Cygwin 3.6.5-1；上游未正式支持 Windows，本项目按固定产物逐项验证，不推定其他构建兼容。
- iperf3 许可原文见 `LICENSE`；Cygwin 许可说明及 GPLv3/LGPLv3 原文见 `CYGWIN-LICENSE`、`CYGWIN-COPYING`、`CYGWIN-COPYING.LIB`。
- Cygwin 许可文本保留自公共源码镜像 `mirror/newlib-cygwin` 的提交 `1b7c72fdcc4bde7520407d2d3364146f04fb8312`。该镜像截至 2024-09-19，不冒充 3.6.5 对应源码；最终分发前需另核对所带运行时的对应源码/补丁提供方式。[Cygwin 官方源码入口](https://cygwin.com/git.html)。

文件摘要见 `SHA256SUMS.txt`。构建和程序运行均不联网下载依赖；最终发布 ZIP 按用户安排另行生成。
