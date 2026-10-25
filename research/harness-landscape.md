# Agent Harness、编排框架与互操作协议：一手资料研究笔记

检索日期：2026-10-21（Asia/Shanghai）。本文服务于整体 Agent 设计报告，不是市占率排名。下列引用均以 Python `urllib.request` 对官方 URL 实际发送 HTTP 请求，成功返回 HTTP 200 并读取正文；记录的是检索时可见文档，不以发布时间不明的网页证明某历史日期已经存在某功能。功能状态以本文标出的官方说明为准。

## 1. 分类：不能把产品、Harness、框架和协议列成同一层

以下是本报告采用的分析定义，而非声称行业已有统一术语标准：

| 层级 | 解决的问题 | 代表对象 |
|---|---|---|
| Agent 产品 | 面向用户完成任务，提供 IDE/CLI/Web/桌面体验 | Claude Code、OpenHands CLI/Cloud 等 |
| Agent Harness / 运行时 | 包围模型决策循环，提供工具执行、上下文、权限、会话、恢复与观测 | Claude Agent SDK、OpenHands Software Agent SDK、Deep Agents |
| 编排框架 | 定义状态、节点、路由、并行、人工介入和恢复机制 | LangGraph、Microsoft Agent Framework Workflows、Google ADK |
| 集成/应用框架 | 组装模型、工具、middleware 与常见 Agent loop | LangChain 等 |
| 互操作协议 | 约定两个组件之间的消息与能力边界 | MCP、A2A、AG-UI、Agent Client Protocol |

边界有重叠。例如 LangGraph 官方称自己是低层编排框架和 runtime；LangChain 当前 `create_agent` 文档也使用 minimal harness 的说法。因此报告应按“承担什么职责”比较，避免把 Harness 当成一种与框架互斥的产品类别。[S1][S2]

## 2. 各家公开事实与设计分析

### LangChain / LangGraph / Deep Agents

**公开事实。** LangGraph 官方将自己定位为构建长时间运行、有状态 Agent 的低层编排框架与 runtime，支持把确定性代码与模型决策放进同一张图；使用 LangGraph 不强制依赖 LangChain。其文档对整个栈的区分是：LangChain 提供模型、工具和 agent loop 抽象；LangGraph 提供 durable execution、streaming、human-in-the-loop 和 persistence；Deep Agents 在其上补充文件系统、上下文管理、subagents 等更完整的 harness；LangSmith 是观测、评估和部署平台。[S1][S2][S3]

Deep Agents 当前概览列出工具与环境操作、上下文压缩和大型结果外置、subagent 隔离上下文、人工审批等；“隔离上下文”表示模型上下文隔离，不能据此推导 OS 进程、文件系统或网络隔离。沙箱文档将环境隔离作为独立 backend 能力，并区分“Agent 整体放进沙箱”和“Agent 在外、沙箱作为执行工具”两种模式。[S3][S4]

**设计分析。** 这套体系适合要定制流程与可靠性的团队，但不能把 `compile(checkpointer=...)` 当作完整生产级保证。开发者仍要选择持久化后端、划定副作用边界、制定重试与人工恢复语义、隔离不可信工具。新产品应把这些决策变成可检查的默认行为，而不是只提供节点 API。

### Claude Agent SDK

**公开事实。** 官方将其描述为把 Claude Code 的工具、agent loop 和 context management 暴露给 Python / TypeScript 应用；SDK 运行 Claude Code binary。它与直接调用模型 API 的 Client SDK 有区别，也与用户终端里的 Claude Code CLI 有区别。文档明确列出 built-in tools、hooks、subagents、MCP、permissions、sessions、skills 与 plugins。[S5]

权限由模式、规则、hooks 和 `canUseTool` 等机制控制。插件可以打包 skills、agents、hooks、MCP servers 等组件；打包结构和加载方式不等于为这些组件创建隔离边界。[S6][S7]

**非常重要的公开限制。** 文件 checkpoint 只追踪通过 `Write`、`Edit`、`NotebookEdit` 产生的修改；Bash 修改不受追踪，子 Agent 编辑也有范围限制。文件 rewind 不会回退对话；创建、移动、删除目录及远程文件也有明确限制。因此“恢复会话”“回退文件”“撤销外部系统动作”是三件不同的事。[S8]

Claude Code 内置 sandbox 文档明确说明其边界是 shell 命令与子进程，文件工具、MCP servers 和 hooks 不在该 shell sandbox 内；页面同时注明 macOS/Linux/WSL2 支持该机制，而原生 Windows 上的命令不受这个 shell sandbox 保护。要把这些组件置于共同边界，需要把整个 Claude Code 进程放进 container、VM 或相应 sandbox runtime。[S9]

**设计分析。** 高度集成的 harness 能显著缩短开发时间，但要接受供应商语义、分发与商业条款边界。其 SDK 库、底层 binary、模型服务应分别检查许可证，不能笼统宣称整个 SDK “闭源”或“开源”。对新 Agent 的最大启示是：UI 必须分别展示“已批准动作”和“实际 OS 隔离范围”，并明确哪些变更能够回退。

### Microsoft Agent Framework / AutoGen / Semantic Kernel

**公开事实。** Microsoft 官方明确称 Agent Framework 是 AutoGen 和 Semantic Kernel 的 direct successor，由相同团队创建：结合 AutoGen 的 Agent/多 Agent 抽象与 Semantic Kernel 的 session state、type safety、middleware、telemetry 等企业能力，并加入显式 graph workflows。不能再把三者写成彼此毫无关系、都应从零开始选择的平行新项目。[S10]

官方当前概览包含 Agents、Harness Agent、Workflows、Integrations 四部分。迁移指南具体描述 workflow checkpoint 可保存 executor state、跨 executor 状态、pending message queues 与 workflow position，并提供列表、检查与恢复 checkpoint 的 API；human-in-the-loop 使用 request-response 工作流接口。[S10][S11]

**设计分析。** 它适合已有 Microsoft/.NET/Azure 应用或要统一 Agent 与显式 workflow 的团队。新架构可学习其把 agent 与 workflow 分开的做法，但框架状态恢复不等于对远端副作用的事务保证；迁移涉及状态 schema、工具契约和行为差异，不能只按 API 名称替换。

### Google ADK

**公开事实。** 官方当前主页定位 ADK 为开源 Agent 开发框架，覆盖模型、工具、工作流、sessions/events/state、runtime、观测与评估，并公布多种语言入口；具体功能须按各语言文档核实，不能由总览推导各语言功能完全对等。[S12]

Resume 文档说明需要配置 resumability；Custom Agents 默认不自动支持，开发者必须按恢复协议改写其代码。Action Confirmation 页面将功能标为 experimental，并在已知限制中列出 `DatabaseSessionService` 和 `VertexAiSessionService` 不受支持。这是检索时页面的明确限制，不能把“支持确认”和“支持生产数据库 session”自动组合为“持久化审批已完整可用”。[S13][S14]

ADK Plugin 是注册在 Runner（Python 也可在 App）上的全局 callback 模块，跨受管理的 agent/tool/model 执行；与局部 agent callback 不同。该“插件”主要是运行时扩展钩子，并不因为名为 Plugin 就具备不可信代码隔离。[S15]

**设计分析。** 成熟 Agent 产品不仅需要一张功能清单，更需要一张“功能组合矩阵”：resume × approval × persistent store × language × deployment backend。应测试组合而不是逐个声称支持。可恢复审批应以独立持久化实体实现，不依赖运行中 Python 栈或 UI tab 存活。

### OpenHands Software Agent SDK

**公开事实。** 官方称其为面向 coding agents 的 Python 与 REST APIs，是 OpenHands CLI 和 Cloud 的引擎；提供 agent、tools、workspace、agent server 等部件，工具覆盖 shell、文件编辑、浏览器与 MCP。主页宣称 SDK 为 MIT 许可；具体部署组件、镜像与可选依赖仍应分别核查。[S16]

Conversation persistence 可把会话、事件、配置、执行状态、工具结果等保存后重载；Security API 把 confirmation policy 与 security analyzer 分开。DockerWorkspace 是独立执行 backend，用于在 Docker 容器中运行 Agent Server。插件打包 skills/hooks/MCP config/agents/commands，文档声称其结构兼容 Claude Code plugin structure。[S17][S18][S19][S20]

**设计分析。** 这是研究“开放、可替换模型的 coding harness”很有价值的参考。仍需区分 conversation state、workspace snapshot、容器生命周期和外部系统效果。Docker 容器的实际隔离强度取决于挂载、权限、网络与内核；不能把文档中的宣传性 “complete isolation” 写成无条件安全承诺。插件文件格式兼容也不表示所有 hook 的行为、安全语义与能力完全相同。

## 3. Durable execution 的可靠性边界

### 3.1 Checkpoint、replay、rollback 是不同能力

**公开事实。** LangGraph checkpoint 当前提供三档 durability：[S21]

| 模式 | 官方语义 | 产品应如何表述 |
|---|---|---|
| `exit` | 结束、错误退出或人工 interrupt 时持久化，中间状态不保存 | 不能保证进程崩溃后的中途恢复 |
| `async` | 执行下一步时异步持久化 | 崩溃时存在最近 checkpoint 尚未写入的窗口 |
| `sync` | 下一步前同步持久化 | checkpoint 更可靠，但仍不把外部 API 动作纳入本地事务 |

LangGraph Functional API 的 resume 并非从断点指令地址恢复；它从 entrypoint 开始 replay，通过持久化结果复用已完成 task/subgraph。随机值、API 调用和文件写入等非确定性工作应放进 task；已开始但未完成的 task 仍可能重新执行，官方建议 idempotency keys 或核查已有结果。[S22]

`interrupt()` 恢复会重新运行所在节点；在 interrupt 前执行副作用可能重复，官方建议幂等操作、把副作用放在确认后、或拆成独立节点。[S23]

**架构启示。** 新 Agent 应分别提供：

1. **状态恢复**：恢复任务、计划、工具调用状态和审批。
2. **审计回放**：读取已经记录的输入、输出与事件来解释发生了什么，默认不再触发动作。
3. **重新执行/分支**：显式创建新 run，使用记录输入或新模型决定，并重新判断授权。
4. **文件回退**：按工作区 diff、快照或版本控制撤回可逆修改。
5. **外部补偿**：例如取消订单、撤销部署；其可行性由外部 API 决定，不能承诺通用“撤销”。

### 3.2 为什么不能无条件保证外部动作 exactly once

**推导，非某一框架的独有缺陷。** 假设 Agent 请求外部系统“发送消息”，外部系统成功执行，但 Agent 在持久化回执前崩溃。恢复后仅凭本地日志无法区分“外部未执行”与“外部已执行但响应丢失”。盲目重试可能重复，不重试可能漏掉。仅增加本地 checkpoint、WAL 或事件日志无法消除这两个系统间的确认窗口。

AWS Builders' Library 的官方实践是使用调用方提供的 request identifier，并强调服务端 token 记录与资源变更必须具有原子性；同一 token、不同意图也需要处理。这为“幂等必须由目标系统参与”提供一手依据。[S24]

建议对每个工具声明 `read_only / idempotent_write / non_idempotent_write`，为写操作使用稳定 `operation_id` 与参数摘要；当目标支持时发送 idempotency key。状态使用 `prepared → dispatched → succeeded/failed/unknown`。崩溃后遇到 `unknown`，先按 operation ID 查询远端；可确认则 reconcile，不能确认且不能安全重试则暂停请求人工判断。outbox 解决“本地状态与待发送意图一致提交”，本身不解决不合作目标的重复副作用。

若目标提供原子去重、事务或可核验的业务操作 ID，可以在明确范围和有效期内实现 effectively-once / exactly-once effect；报告不要把“不存在无条件全局 exactly-once”误写成“任何条件下都绝不可能”。

## 4. 协议应放在哪条边界

| 协议 | 公开事实与接口边界 | 新 Agent 应采用的方式 |
|---|---|---|
| MCP | JSON-RPC host/client/server 架构；server 暴露 tools/resources/prompts；host 承担权限、连接与上下文管理。[S25] | 接入工具与数据的 adapter；不把协议等同于插件沙箱或包管理器 |
| A2A | Agent Card 描述能力与接口，提供任务/消息/artifacts、状态、流式或异步通知及认证相关约定。[S26] | 当需要调用独立远端 Agent 时引入；无需用它包裹所有内部函数 |
| AG-UI | 面向 Agent backend 与用户应用的事件协议，覆盖消息流、状态、工具、interrupt 等交互。[S27] | Web/桌面 UI 的可选桥接；内部领域事件需有自己的版本与持久化契约 |
| Agent Client Protocol（ACP） | 面向代码编辑器/client 与 agent；JSON-RPC，包含 session/prompt、update、request_permission，及可选文件/终端能力。典型 agent 作为 client 的子进程运行。[S28] | 接入 IDE 与编辑器；具体版本协商，避免混用 stable 与 draft |

ACP 缩写存在歧义，报告必须写出 **Agent Client Protocol** 全称和 `agentclientprotocol.com` 来源。本表没有把历史上其他名为 Agent Communication Protocol 的项目混入。

**安全边界的公开事实。** MCP host 负责安全策略；tools 规范要求把不可信 server 给出的 annotations 当作不可信内容。不能因为插件自称 `readOnlyHint` 或“safe”就直接授予权限。[S25][S29] Roots 是协议级目录范围与协作约定；真正阻止任意本地进程读取文件，仍必须依赖 OS/容器/VM 权限，不能靠 JSON 中的路径声明。[S30，后半句为架构分析]

## 5. 对新插件系统的具体启示（设计建议）

- **四类扩展分别承载**：声明式 skills/prompts；远端或本地 RPC 工具；可执行 hooks/worker；UI 扩展。不要把一份 Markdown 和一个任意原生动态库授予相同权力。
- **Manifest 是权限请求，策略服务才是授权者**：声明版本、入口、publisher、摘要、依赖、工具 schema、路径/网络/密钥/CPU/内存配额、支持的宿主协议版本；实际授权为“用户/组织授权 ∩ workspace policy ∩ plugin requested capabilities”。
- **默认跨进程**：工具插件使用受监督 worker + JSON-RPC/本地 socket；分配每个调用的 token/handle，而不是宿主全部环境变量、文件路径和密钥。单独进程只提供故障隔离，OS 沙箱才提供权限隔离。
- **隔离不可由插件关闭**：hooks 与 MCP 子进程都纳入执行策略；如要在宿主内运行可信第一方模块，单独标识并缩小接口面。不要允许插件通过“自动化便利”扩展自身权限。
- **授权绑定具体动作**：approval 包含 tool ID、规范化参数摘要、目标 workspace revision、权限范围、有效期与 policy revision。参数、工作区或策略变化后再次检查，避免审批完工具悄悄换内容。
- **失败是协议一部分**：请求具备 operation ID、deadline、cancellation、progress、result/error schema、幂等性声明与恢复策略；取消调用不保证已发生副作用自动回滚。
- **生命周期可审计**：安装/更新/权限变更都可追溯；版本锁定、签名/哈希校验、撤销与回滚；插件崩溃不损坏任务日志，插件更新不静默改变进行中的 run。
- **协议放在 adapter 层**：MCP、A2A、AG-UI、ACP 接入统一内部 command/event domain，不让外部协议成为数据库 schema。这样协议升级和多端接入不会重写任务内核。

## 6. 官方来源和 HTTP 核验清单

以下链接全部已实际抓取、返回 200 并读取正文；标注重定向的使用最终地址。未经功能集成测试，不应把“文档可读”写成“已验证产品行为”。

- **S1** LangGraph overview：https://docs.langchain.com/oss/python/langgraph/overview
- **S2** LangChain overview：https://docs.langchain.com/oss/python/langchain/overview
- **S3** Deep Agents overview：https://docs.langchain.com/oss/python/deepagents/overview
- **S4** Deep Agents sandboxes：https://docs.langchain.com/oss/python/deepagents/sandboxes
- **S5** Claude Agent SDK overview：https://code.claude.com/docs/en/agent-sdk/overview （由 `platform.claude.com/docs/en/agent-sdk/overview` 重定向）
- **S6** Claude Agent SDK permissions：https://code.claude.com/docs/en/agent-sdk/permissions
- **S7** Claude Agent SDK plugins：https://code.claude.com/docs/en/agent-sdk/plugins
- **S8** Claude Agent SDK file checkpointing：https://code.claude.com/docs/en/agent-sdk/file-checkpointing
- **S9** Claude Code sandboxing：https://code.claude.com/docs/en/sandboxing
- **S10** Microsoft Agent Framework overview：https://learn.microsoft.com/en-us/agent-framework/overview/ （旧 overview 路径重定向）
- **S11** Microsoft AutoGen migration guide：https://learn.microsoft.com/en-us/agent-framework/migration-guide/from-autogen/
- **S12** Google ADK overview：https://adk.dev/ （由 `google.github.io/adk-docs/` 重定向）
- **S13** ADK resume：https://adk.dev/runtime/resume/index.md
- **S14** ADK action confirmation：https://adk.dev/tools-custom/confirmation/index.md
- **S15** ADK plugins：https://adk.dev/plugins/index.md
- **S16** OpenHands Software Agent SDK：https://docs.openhands.dev/sdk
- **S17** OpenHands persistence：https://docs.openhands.dev/sdk/guides/convo-persistence.md
- **S18** OpenHands security/confirmation：https://docs.openhands.dev/sdk/guides/security.md
- **S19** OpenHands Docker workspace：https://docs.openhands.dev/sdk/guides/agent-server/docker-sandbox.md
- **S20** OpenHands plugins：https://docs.openhands.dev/sdk/guides/plugins.md
- **S21** LangGraph checkpointers：https://docs.langchain.com/oss/python/langgraph/checkpointers （原 durable-execution 页现重定向至总览 persistence，持久化分档具体见本页）
- **S22** LangGraph Functional API：https://docs.langchain.com/oss/python/langgraph/functional-api
- **S23** LangGraph interrupts：https://docs.langchain.com/oss/python/langgraph/interrupts
- **S24** AWS Builders' Library, Making retries safe with idempotent APIs：https://aws.amazon.com/builders-library/making-retries-safe-with-idempotent-APIs/
- **S25** MCP 2025-11-25 architecture：https://modelcontextprotocol.io/specification/2025-11-25/architecture
- **S26** A2A specification：https://a2a-protocol.org/latest/specification/
- **S27** AG-UI overview：https://docs.ag-ui.com/introduction
- **S28** Agent Client Protocol v1 overview：https://agentclientprotocol.com/protocol/v1/overview （`/protocol/overview` 当前重定向至 v1；导航另有 v2 Draft，本文未将草案当稳定版）
- **S29** MCP 2025-11-25 tools：https://modelcontextprotocol.io/specification/2025-11-25/server/tools
- **S30** MCP 2025-11-25 roots：https://modelcontextprotocol.io/specification/2025-11-25/client/roots

研究过程遇到的失效入口：Microsoft `/user-guide/workflows/checkpointing-and-resuming` 与 `/workflows/checkpointing-and-resuming` 返回 404；本文改用可读的官方迁移指南 S11，未把失效链接用作证据。
