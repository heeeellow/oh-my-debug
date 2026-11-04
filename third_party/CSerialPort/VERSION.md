# CSerialPort

- 上游：<https://github.com/itas109/CSerialPort>
- 固定提交：`cb8c8b4bb358c80a01f0605367a30b07643863cb`
- 许可：[LGPLv3 及上游附加例外](LICENSE)，保留完整原文。
- 保留 `include/`、`src/`、上游 `VERSION` 和许可证；桌面工程直接编译所需源文件。
- 本地修改：增加可检查返回结果的控制线接口 `setControlLineChecked`，Windows 拒绝与握手冲突的设置并报告失败；保留原 void 接口。
- 不纳入上游示例、多语言绑定、测试和独立构建文件；完整上游可按固定提交取得。
- 当前保留文件（含本地修改）的摘要见 [SHA256SUMS](SHA256SUMS)。
