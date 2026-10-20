# TestTools 开发接手规则

本文是开发者和自动化编码工具进入仓库时的首要入口。规则保持工具无关；当前实现状态以
[`docs/STATUS.md`](docs/STATUS.md) 为准，不能根据历史日志猜测。

## 文档归属

本项目不接入共享知识库（Note_Base）。设计、开发、审查和验证记录只维护在本仓库的 `docs/` 中。
不得为本项目在共享知识库创建或更新项目页、Review、索引或其他记录；即使使用调试工作流，
也不得执行其知识库归档步骤。

## 开始任务前

按任务读取最小文档集：

1. 先读 [`README.md`](README.md) 和 [`docs/STATUS.md`](docs/STATUS.md)。
2. 修改产品行为或界面时，再读 [`docs/PRODUCT.md`](docs/PRODUCT.md)、
   [`docs/UI_DESIGN.md`](docs/UI_DESIGN.md) 和对应的 `docs/features/` 功能契约。
3. 修改分层、协议或板端能力时，再读 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) 和
   [`docs/AGENT_PROTOCOL_V1.md`](docs/AGENT_PROTOCOL_V1.md)。
4. 构建、验证和新增模块遵循 [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)。
5. 只有追溯原因或历史验证时才读取 [`docs/DEVLOG.md`](docs/DEVLOG.md)，历史结果不能替代当前验证。

开始编辑前必须检查 Git 状态和相关差异，保留所有无关的用户修改。不得擅自暂存、提交、改写历史
或推送；提交信息统一使用中文。

## 不可破坏的工程边界

- Qt 只用于 `src/ui/` 的表现和 GUI 线程桥接；`src/core/`、`src/application/` 和板端 Agent
  不得依赖 Qt 类型。
- ADB 只负责设备发现、部署和回环端口转发，不承载串口或测试业务语义。
- 板端 Agent 不提供任意 shell RPC；能力必须有固定 ID、参数校验、资源声明和风险边界。
- GUI 线程不得执行阻塞式 ADB、网络或串口操作；后台回调不得直接访问 Qt 控件。
- 通用串口行为不能由设备名称暗中改变；协议相关行为必须通过显式配置或独立协议工具提供。
- 普通定时/次数发送与可靠性压测是不同功能，参数、状态和结果不得耦合。
- 真实发送、存储写入、重启、压力测试等设备操作必须区分于只读或无发送检查。

## 完成任务的最低要求

- 行为变化同步更新唯一权威文档和对应验收条件，不在多个文件复制当前状态。
- 核心逻辑优先在无 Qt 层验证；界面变化至少检查目标分辨率快照、控件状态和异常路径。
- 构建、静态检查、实机、异常、回归和压力验证分别记录，未执行的项目明确写“未验证”。
- 验证结果与交付状态分开；本地通过不代表已提交，已提交也不代表实机通过。
- 结束前运行 `scripts/check-project-docs.ps1`，检查文档入口和相对链接，并复核 Git 差异范围。
