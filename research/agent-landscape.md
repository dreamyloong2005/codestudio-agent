# 开发者 Agent 产品格局与痛点：官方资料研究摘录

研究日期：2026-10-21，Asia/Shanghai。本文件属于研究材料，供最终报告引用。所有下列来源均在本次会话通过 HTTP 实际抓取成功；以返回正文为依据，不仅使用搜索摘要。这里的“代表产品”不表示市场份额排名，也不是对全部 2026 年产品的穷尽。未进行同题任务、成本、延迟或安全对抗实测。

## 关键判断

1. **“模型 + 工具调用”已经不是足够的差异化。** 本次核实的多个产品已经提供子代理、持久会话、技能、MCP、hooks、插件、桌面/CLI/IDE 多入口和云端执行中的多项能力。不能把“支持插件”“支持多代理”“支持 checkpoint”写成尚无人解决的问题。
2. **竞争正在从编辑器补全，转向跨入口、跨环境的工程执行平台。** Cursor Cloud Agents 已支持多仓库、完整 VM、浏览器/桌面操作及截图视频证据；Claude Code、Codex 已跨多个入口；OpenHands 当前主仓定位已是 Agent Canvas 控制中心。
3. **“有恢复按钮”与“任务可以正确恢复”不同。** Claude 官方明确说明 checkpoint 不覆盖 Bash 修改，通常不恢复其他子代理首次修改的文件，也不等于 Git；Cursor 的 checkpoint 只恢复文件、不删除会话消息；云端任务还有时长、网络、凭据和环境差异。新产品的可靠性设计应以可重放事件、明确副作用边界和可验证结果为中心。
4. **真正值得验证的产品机会，是跨供应商的统一恢复、权限解释、结果证据、环境迁移和成本控制。** 这是本文基于公开差异得出的产品判断，不能声称现有产品完全没有这些能力，更不能仅凭文档推出新产品会胜出。

## 产品矩阵

| 产品/形态 | 本次官方资料确认的能力与定位 | 明确边界或仍需验证的问题 | 主要来源 |
|---|---|---|---|
| **OpenAI Codex**：CLI、IDE、桌面与云端工作流 | CLI 可检查/修改代码、执行本地命令、非交互 CI、恢复会话、代码审查、子代理及 MCP。插件可封装 skills、MCP servers、browser extensions、hooks。沙箱将技术执行边界与审批策略分开，并使用 OS 原生机制。 | 插件支持依入口而异，文档明确 IDE extension 不支持插件；Desktop only 插件、云端 hooks、OAuth 能力也有差异。不能把“同一插件目录”理解成所有入口都等价执行。此处未验证跨供应商无损会话迁移，也不评价实际成功率。 | [A1][A2][A3] |
| **Anthropic Claude Code**：CLI、IDE、桌面与 Web | 官方已公开文件编辑、命令执行、Git/PR、MCP、CLAUDE.md/AGENTS.md、自动记忆、skills/hooks、并行子代理、Agent SDK、计划任务及部分跨入口移交。插件可含 skills、agents、hooks、JavaScript hooks module/mods、MCP。 | Checkpoint 不追踪 Bash 修改；一般子代理产生的新文件修改不随主会话 rewind 全量恢复；外部并发修改也不是完整跟踪对象。已启用插件的描述占用上下文，MCP 进程与 hooks 有运行成本；插件代码以用户权限运行。 | [A4][A5][A6] |
| **Cursor Agent / Cloud Agents**：编辑器及云端工程环境 | 本地 Agent 有代码/网页搜索、文件编辑、终端、浏览器、澄清问题、checkpoint；项目协调 Agent 可规划并委派工作。Cloud Agents 运行于隔离 VM，支持多仓库、MCP、hooks、截图/视频/日志和远程桌面接管；已支持多种源码托管平台。 | 本地 checkpoint 恢复文件，不删除聊天消息。云端能否闭环依赖环境、依赖、secret 和网络可用性；某些 hooks 在早期只读探索阶段不执行，云端不读取用户本地 home hooks。云端按模型 API 价格计费，较大上下文可能增加成本。不能再将其描述为仅支持 GitHub 或单仓库。 | [A7][A8] |
| **GitHub Copilot**：IDE/CLI 与 GitHub Cloud Agent 等不同入口 | GitHub.com 可在对话中研究、规划、委派背景任务，云端临时开发环境执行编辑、测试和 lint；可创建分支/提交/PR，支持自动化、custom agents、skills、MCP、hooks 和会话日志。 | **本次所查 Cloud Agent 文档**明确：一次运行只改指定仓库、一次只处理一个分支/一个 PR，最长运行 **59 分钟**，且仅支持 GitHub 托管仓库。这些限制不能泛化为所有 Copilot 入口的限制。官方也明确日志不能代替用户审查与测试。 | [A9] |
| **Google Gemini CLI**：可扩展终端 Agent | 官方文档公开 extensions、skills、MCP、hooks、headless、checkpoint/rewind、记忆、模型选择与 fallback；子代理具有独立上下文，可限定工具；沙箱包含 macOS、容器、Windows 原生及 Linux 专属选项。 | 平台和沙箱后端不等价；容器方案需要已有 Docker/Podman 环境。浏览器子代理默认关闭，容器内通常需连接宿主 Chrome；部分视觉能力受身份认证方式限制。文档总览对部分功能带实验标记，不能凭总览推断所有特性同等成熟。 | [A10][A11][A12] |
| **Google Antigravity**：桌面控制中心、CLI、IDE 与 SDK | 官方概览已描述从 Agent Manager 演进为独立桌面 command center，另有终端、IDE 扩展及 Python SDK；支持交互配对编程和异步背景工作、多代理编排。 | 本次只用官方概览确认产品形态与公开能力，未核验其恢复语义、插件隔离强度、跨模型结果一致性或长期任务成功率。概览中的演示模型列表不用于证明账号实际可用性。 | [A13] |
| **Devin**：可委派软件工程任务与本地/云端衔接 | 官方介绍包含写/运行/测试代码、并行处理工单、迁移重构、浏览器、可接管 IDE、API；Devin for Terminal 可将长任务 handoff 到云端。 | 官方明确建议提供完成标准、使任务容易验证并拆分复杂任务；其“多数三小时内人工能做的任务”等表达属于厂商经验宣传，本报告不作为独立性能证据。介绍页本身提醒文档可能落后于实际行为。 | [A14] |
| **Windsurf / Cascade，当前文档归入 Devin Desktop** | 原 Windsurf Cascade 文档已 HTTP 重定向至 Devin Desktop。当前正文称 Cascade 是两个本地 Agent 之一，另一个是 Devin Local；公开 Code/Chat 模式、规划、MCP、terminal、memory/rules、workflows、checkpoint 及并发会话。 | 官方明确同文件并发编辑可能竞争或失败，建议使用 worktree；checkpoint reverts 当前不可逆。默认每 prompt 最多 40 次工具调用，auto-continue 会继续消耗相应 prompt credit。品牌关系这里只按实际文档归属描述，不推测收购或完整产品整合范围。 | [A15] |
| **OpenHands / Agent Canvas**：开源 Agent 控制中心与执行后端 | 当前主仓 README 已介绍 Agent Canvas：本地/远程/云多后端、自托管、定时或 webhook 自动化；默认支持 OpenHands Agent，也可经 ACP 使用 Claude Code、Codex、Gemini CLI、Pi、OpenCode 等。后端为 Agent Server，SDK 与 UI、automation 各有独立仓库。 | README 的无沙箱启动方案使 Agent 获得宿主文件系统完整访问；按会话 Docker 隔离不等于整个 Canvas/automation 都被隔离；不同容器若挂载同一宿主工作区，仍会文件冲突。由此可见“有容器”不足以证明完整隔离。 | [A16] |
| **OpenCode 2**：开源多提供商、CLI/桌面/Web | V2 官方入口确认 CLI、desktop、Web 配对界面、多 LLM 提供商、插件、MCP 和自定义 commands；提供各主流 OS 二进制。 | V1/V2 共用默认 `opencode` 命令，旧入口明确不能按默认命令并存；V2 文档指出 Windows 包管理器不受支持，需对应独立二进制等方式。多提供商支持并不自动证明工具调用、压缩与恢复语义完全兼容。 | [A17][A18] |
| **Qwen Code**：开源多协议、多端 Agent | 官方当前 README 列出 Auto-Memory/Auto-Skills、SubAgents/Agent Teams、MCP；支持 OpenAI、Anthropic、Gemini、Qwen API 及本地模型，拥有终端、桌面、IDE、Web、聊天入口，以及 TypeScript/Python/Java SDK。 | Web UI/daemon 在 README 标作 experimental。其对 Claude 的 feature parity 表和 SWE-bench 分数属于厂商自报，本次不据此宣布优劣或无锁定保证。“能连接多协议”与“切换后行为等价”仍需测试。 | [A19] |
| **Kimi Code CLI**：当前新版终端 Agent | 新仓 README 确认代码/文件/命令/网页工具、兼容提供商、插件市场、MCP 配置、独立上下文子代理、hooks、ACP 编辑器连接及单二进制发行。 | 旧 Python Kimi CLI 仓库已归档并明确替换为新版；迁移可搬配置、MCP server 定义、输入历史和会话，但登录凭据、MCP 授权和旧插件不迁移。新版 Windows 当前依赖 Git Bash。性能宣传未独立实测。 | [A20][A21] |

## 从已证实限制提炼的共性痛点

### 1. 任务状态、代码状态与外部世界状态经常不是一回事

官方 checkpoint 限制已经证明：恢复会话、恢复编辑器文件、回滚 shell 命令、撤销已发送消息或云端 API 操作，是不同层级的问题。新产品可以统一展示这些层级，但不能承诺任何副作用都可撤销。对外部操作应记录 intent、审批、幂等键、执行结果与补偿方式；发生中断后先核对事实，再决定是否重试。

### 2. 多代理可用，不代表并发编辑和整合已经可靠

Cascade 直接写明同文件编辑竞争；OpenHands 说明容器挂载同一宿主目录仍共享文件。可验证的改进方向是工作区隔离、修改所有权、基于提交的整合、冲突检测及独立结果验收，而不是把更多子代理本身当作进步。

### 3. 入口、身份与执行环境差异容易变成用户负担

Codex 的插件按入口分级；Claude 的本地插件不自然等同云端配置；Cursor 的 secrets、hooks 和环境存在云本地差异；Gemini 的浏览器能力受沙箱与认证方式影响；Kimi 新旧版本迁移需要重新授权。新 Agent 应提供“实际能力清单 + 迁移预检 + 未迁移项清单”，不是只展示一个理论上统一的聊天界面。

### 4. 插件既是生产力入口，也是成本与信任边界

Claude 插件文档明确描述上下文 token、每会话服务进程、hooks 与用户权限。因而插件系统需要可见的权限、懒加载、资源限额、版本锁定及来源校验；“遵循 SKILL.md”并不能替代操作系统沙箱。“跨产品插件兼容”也应明确定义支持哪些组件、哪些生命周期事件、如何降级。

### 5. 交付证据的重要性超过自然语言的完成宣称

Cursor 已有截图/视频/日志，GitHub 已有 session logs，不能称证据展示是市场空白。更进一步的机会是把证据与提交 SHA、测试环境、命令退出状态、需求验收条件绑定，区分“测试通过”“部署完成”“外部行为已验证”，并保留未验证项。

### 6. 成本、运行上限和成功率需要作为产品控制项

Copilot Cloud Agent 的 59 分钟限制、Cascade auto-continue 的 credit 消耗、Cursor 按 API 模型计费与 Claude 插件上下文开销，说明时间与费用预算必须显式进入调度器。可设计每任务/每子任务的 token、费用、时长、并发上限，以及停止/降级理由。本文未获得跨厂商同口径价格与成功率数据，故不做“最便宜”“最强”排名。

## 可复核来源

以下每项检索日期均为 **2026-10-21（Asia/Shanghai）**，均已成功抓取正文（HTTP 200，部分为最终重定向地址）。官方 README 属于一手材料，但其中营销判断和 benchmark 自报未被当作独立结论。链接中的 Markdown 后缀仅用于抓取同一官方文档正文。

- **[A1] Codex CLI**：https://learn.chatgpt.com/codex/cli.md 。支持本地编辑执行、CI、resume、子代理、cloud、MCP。`https://developers.openai.com/codex/` 在本次重定向至 `https://learn.chatgpt.com/docs`。
- **[A2] Codex Plugins**：https://learn.chatgpt.com/codex/plugins.md 。支持插件组成、统一目录、入口差异、IDE 不支持插件、hooks 与身份限制。
- **[A3] Codex Sandbox**：https://learn.chatgpt.com/codex/sandboxing.md 。支持沙箱/审批分离、OS 机制及配置；不证明所有平台实现完全等价。
- **[A4] Claude Code Overview**：https://code.claude.com/docs/en/overview.md 。支持各入口、自动记忆、多代理、SDK、定时任务与跨入口工作流；其“理解整个代码库”等表述不当作成功率证据。
- **[A5] Claude Code Checkpointing**：https://code.claude.com/docs/en/checkpointing.md 。支持 Bash、子代理、外部修改、符号/硬链接等恢复限制。
- **[A6] Claude Code Plugins overview**：https://code.claude.com/docs/en/plugins.md 。支持插件组成、上下文/进程成本、用户权限、云本地安装差异。
- **[A7] Cursor Agent Overview**：https://cursor.com/docs/agent/overview 。支持工具、协调 Agent、checkpoint 与文件/消息恢复边界。
- **[A8] Cursor Cloud Agents**：https://cursor.com/docs/cloud-agent 。支持 VM、多仓库、多源码托管平台、MCP/hooks、证据、环境及计费约束。
- **[A9] GitHub Copilot on GitHub.com**：https://docs.github.com/en/copilot/concepts/copilot-surfaces/copilot-on-github 。原 `.../agents/coding-agent/about-coding-agent` 自动重定向至此；支持 Cloud Agent 工作流及单仓库/单分支/单 PR/59 分钟限制。
- **[A10] Gemini CLI documentation**：https://geminicli.com/docs/ 。支持能力目录。页面自带更新时间为 2026-04-17，本次另读详情页，不把目录更新时间当成本次产品实测时间。
- **[A11] Gemini CLI Sandboxing**：https://geminicli.com/docs/cli/sandbox/ 。支持平台/容器前提和沙箱差异。
- **[A12] Gemini CLI Subagents**：https://geminicli.com/docs/core/subagents/ 。支持独立上下文、工具限制、浏览器子代理默认关闭及沙箱/认证限制。
- **[A13] Google Antigravity overview**：https://antigravity.google/docs/overview 。原 `/docs/home` 返回跳转页面，本次进一步实际抓取 overview；支持多产品入口与多代理公开定位，不证明具体安全或恢复保证。
- **[A14] Introducing Devin**：https://docs.devin.ai/get-started/devin-intro.md 。支持工具、工单/迁移定位、Terminal handoff 和厂商建议；不采纳未独立验证的能力宣传。
- **[A15] Cascade Overview（Devin Desktop）**：https://docs.devin.ai/desktop/cascade/cascade 。由 `https://docs.windsurf.com/windsurf/cascade/cascade.md` 自动重定向；支持当前命名、工具调用限制、不可逆 revert 和并发冲突说明。
- **[A16] OpenHands 官方主仓 README**：https://raw.githubusercontent.com/OpenHands/OpenHands/main/README.md 。支持当前 Agent Canvas/Agent Server/Automation 结构、ACP 后端、沙箱与共享工作区限制。可浏览入口：https://github.com/OpenHands/OpenHands 。
- **[A17] OpenCode 2 Intro**：https://opencode.ai/v2/docs/ 。支持 V2 多端、提供商、plugins/MCP 与发行方式。
- **[A18] OpenCode 1 Intro**：https://opencode.ai/docs/ 。仅用来核实 V1/V2 同名命令与迁移警告，不混用旧版能力推断 V2。
- **[A19] Qwen Code 官方 README**：https://raw.githubusercontent.com/QwenLM/qwen-code/main/README.md 。支持多协议、多端、多代理与 SDK 的公开声明；实验特性与厂商 benchmark 另作限定。浏览入口：https://github.com/QwenLM/qwen-code 。
- **[A20] Kimi Code CLI 新仓 README**：https://raw.githubusercontent.com/MoonshotAI/kimi-code/main/README.md 。支持新版功能、发行、ACP、Git Bash 前提。浏览入口：https://github.com/MoonshotAI/kimi-code 。
- **[A21] Kimi CLI 旧仓归档说明**：https://raw.githubusercontent.com/MoonshotAI/kimi-cli/main/README.md 。支持替换关系与迁移保留/不迁移项目。浏览入口：https://github.com/MoonshotAI/kimi-cli 。

## 研究限制

- 官方文档能确认“厂商公开支持什么、声明限制什么”，不能证明实际缺陷发生频率、用户满意度或跨产品可靠性排名。
- 同名产品的本地、CLI、IDE、云端、企业部署能力经常不同；矩阵有意标注受限的入口。
- 品牌/版本变化依据本次真实页面及重定向记录；不据此扩展成未经核实的商业组织关系。
- 未比较 Cline、Roo Code、Trae 等其他产品；这不是对其重要性或能力的否定。

## 补充：OpenAI Agents SDK 与 Cline

本节同样于 **2026-10-21（Asia/Shanghai）**实际抓取官方正文，补充前述主表；上文“未比较 Cline”仅描述补充前的主表范围。

| 代表项目 | 本次核实的能力 | 工程边界与设计启示 |
|---|---|---|
| **OpenAI Agents SDK**：应用内 Agent runtime / SDK，Python 与 TypeScript | Runner 处理模型—工具—handoff 循环；可用函数工具、MCP、hosted tools；提供 handoffs 与 agents-as-tools 两种编排，输入/输出/工具 guardrails，审批中断及可序列化恢复 state，session 和其他会话延续策略；服务端常规路径默认内置 tracing，记录模型、工具、handoff、guardrail 与自定义 spans。 | SDK 跑在开发者应用内。部署、工具实现、存储、审批决策及运行环境由应用负责，不应把 SDK 等同完整本地编码 Agent 产品，或 OpenAI 托管的 Codex harness。它可集成沙箱，但使用 SDK 不等于已获得 OS 级隔离、工作树管理、插件治理、终端 UI、整套工程验收与迁移能力。官方明确 SDK/API 应用不会自动继承 Codex Auto-review。[A22]–[A27] |
| **Cline**：共享 Agent core 的编辑器、终端、桌面及 SDK | 当前官方概览列出 VS Code、JetBrains、CLI、桌面、SDK；拥有文件、命令、浏览器工具，支持 BYOK 与本地 runtime，ACP 可连接相应编辑器；Kanban 用每卡 worktree、依赖链和 auto-commit 管理并行任务。 | 不能只写成“VS Code 插件”，也不能引用概览的“每步人工批准”概括全部模式：详情页明确可按读/写/命令/浏览器/MCP 类别 Auto Approve，还存在 YOLO 模式。所谓 safe command 由模型设置 `requires_approval`，不是固定安全 allowlist，不能当作独立 OS 安全边界。本文未实测其模型适配质量或恢复可靠性。[A28][A29] |

### Agents SDK 中容易混淆的边界

- **Handoff 与调用子代理工具不同。** Handoff 把当前分支的会话控制权移交给 specialist；agents-as-tools 保留 manager 对最终答复的责任。官方建议只有能力、策略、提示或可观测性边界确实需要改变时才拆分 agent。
- **Session 与暂停中的运行 state 不同。** Session 管理跨轮对话；审批产生 `interruptions` 与 resumable `state`。批准后应恢复同一 run，而不是新建一个用户 turn。把本地完整历史同时与 server-managed continuation 混用可能重复上下文。
- **Guardrails 有执行范围。** Input guardrails 只针对链首 agent，output guardrails 只针对最终输出 agent；tool guardrails 作用于其挂接的函数工具。需要控制每个副作用时，应在工具边界执行策略，不能只依靠首尾检查。
- **Tracing 已存在，不等于事务日志。** SDK 已能记录工具/模型/审批轨迹；“完成后必能精确恢复、避免外部动作重复、证明代码验收通过”仍需应用的持久化与业务协议设计。此句为工程推论，不是声称 SDK tracing 有缺陷。

### 补充来源

全部在本次成功返回 HTTP 200。OpenAI 先做官方域限定主题搜索，再实际读取官方正文；未使用 GitHub 外部 SDK 文档替代这次要求的官方文档。

- **[A22] OpenAI Agents overview**：https://developers.openai.com/api/docs/guides/agents 。支持 Agents API（OpenAI 托管 Codex harness）、Agents SDK（应用内 runtime）、Responses API 的职责对比；`.../guides/agents-sdk` 在本次跳转到此页。
- **[A23] OpenAI Agents SDK**：https://developers.openai.com/api/docs/guides/agents/sdk.md 。支持 Python/TypeScript、tools、运行与沙箱集成入口，以及应用负责部署、工具、存储和审批的边界。
- **[A24] OpenAI Running agents**：https://developers.openai.com/api/docs/guides/agents/running-agents.md 。支持 agent loop、session、history、conversation ID、previous response ID，以及中断后从 state 恢复。
- **[A25] OpenAI Orchestration and handoffs**：https://developers.openai.com/api/docs/guides/agents/orchestration.md 。支持 handoff 与 agents-as-tools 的控制权差别，以及避免过早拆分多代理的建议。
- **[A26] OpenAI Guardrails and human review**：https://developers.openai.com/api/docs/guides/agents/guardrails-approvals.md 。支持 guardrails 范围、审批生命周期、序列化 state，以及 SDK 不自动继承 Codex Auto-review。
- **[A27] OpenAI Integrations and observability**：https://developers.openai.com/api/docs/guides/agents/integrations-observability.md 。支持 hosted/local/private MCP 连接责任，以及 tracing 默认行为与记录对象。
- **[A28] Cline Overview**：https://docs.cline.bot/cline-overview 。原 `https://docs.cline.bot/introduction/welcome` 跳转至此；支持现有各产品入口、共享 core、模型接入、ACP 与 Kanban。
- **[A29] Cline Auto Approve & YOLO Mode**：https://docs.cline.bot/features/auto-approve.md 。支持按工具类别自动批准、模型 `requires_approval` 分类和 YOLO 行为；不以此证明任何 OS 沙箱保证。
