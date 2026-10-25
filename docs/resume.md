# 检查点与任务续跑

`resume` 从同一任务的持久化检查点继续执行。它恢复对话、三种模型协议的原生续传内容、待处理工具、已有回执和已消耗的轮次。任务 ID 和动作记录保持连续。

## 使用

进程崩溃后，先检查和整理中断记录，再续跑：

```powershell
xmake run codestudio-agent history --workspace D:\YourProject
xmake run codestudio-agent show <task-id> --workspace D:\YourProject
xmake run codestudio-agent recover --workspace D:\YourProject
xmake run codestudio-agent resume <task-id> --workspace D:\YourProject
```

原任务使用过的权限必须再次明确提供。例如：

```powershell
xmake run codestudio-agent resume <task-id> --workspace D:\YourProject `
    --allow-write --allow-process 'D:\Tools\xmake.exe'
```

如果原任务连接无 TLS 的回环测试服务，需要再次添加 `--allow-http-loopback`；如果使用 MCP，需要再次提供同一 version 2 本地配置 `--mcp-config` 和全部 `--allow-mcp`。手动续跑不会自动启用未再次提供的权限；后台续跑需单独显式登记原授权。

如果原任务使用 `--sandbox appcontainer`，续跑也必须显式重复该选项；缺省 `none` 会因绑定不匹配而拒绝。不允许在续跑中切换沙箱模式。

固定清单构建需再次提供原 `--build-config`。构建清单与内部执行计划均使用 version 2，恢复时核对同一绑定；所需步骤和环境必须在清单中显式声明。

原任务开放构建提案时，需要再次提供 `--allow-build-proposals`，工具 schema 必须与检查点完全一致。成功提出方案后任务暂停为 `waiting_approval`，CLI 返回 3；提案对应的工具调用暂不回复模型。批准或拒绝后，显式 `resume` 将决定和明确的执行结果回注到该工具调用，再继续原任务。待决、批准构建仍在运行或结果不明时拒绝续跑。批准后创建的独立构建任务没有模型检查点，不支持 `resume`；再次执行需要新提案。详见 [构建提案](build-proposals.md)。

模型、端点、API 类型、流式模式、超时、输出 token 上限和 `credential` 引用默认来自检查点；密钥值在本次续跑开始时按引用重新读取，并固定使用到操作结束。引用可为 `env:VARIABLE` 或 `system:identifier`，新模型任务默认 `env:CODESTUDIO_API_KEY`；绑定只保存引用，不保存值。显式 `--credential` 必须与保存引用相同，缺少引用的旧模型检查点拒绝续跑，不补默认值或转换。云端模型环境变量不会覆盖保存的配置。提供模型或端点参数时必须与原配置一致，不支持在续跑中切换供应商或模型。轮换和平台条件见 [模型凭据与轮换](credentials.md)。

`--max-turns` 是整个任务累计的模型请求预算，包括失败或被中断的请求，范围仍为 1..100。预算耗尽后可以明确扩大：

```powershell
xmake run codestudio-agent resume <task-id> --workspace D:\YourProject --max-turns 16
```

默认沿用上次预算，不会自动追加。已记录、尚未执行的工具会先处理；不会为了续跑重发已完成动作。中断的模型请求可能重新发起并计入下一轮，因此可能再次产生模型费用。

## 允许继续的条件

任务状态必须为 `interrupted`、`cancelled`、`failed`、`exhausted` 或 `waiting_approval`，且拥有受支持的完整检查点。`waiting_approval` 还要求对应提案已有决定、批准执行已经结束且效果明确；拒绝决定不需要构建执行。`running` 必须先 `recover`；`completed` 和 `needs_attention` 不续跑。

工作区、模型配置、工具权限、进程限制、沙箱策略、MCP 清单和工具 schema 必须与原绑定一致。MCP 清单按完整内容比较，修改空白也算变化；允许的工具定义变化同样会拒绝。配置/策略差异会在启动 MCP 前拒绝；随后已授权插件会重新启动以核对工具定义，但宿主不会重放已记录的工具调用。插件启动本身可能有副作用，默认 `none` 模式仍只适用于可信插件。

AppContainer 模式的 scratch 不进入检查点：普通命令每次调用重新创建，MCP 在连接关闭或重启后重新创建。续跑恢复的是宿主对话、配置与回执，不会恢复插件内存或临时文件。依赖连接内状态的插件可能需要向模型报告状态丢失。工作区导入/输出代理尚未实现，详见 [沙箱说明](sandbox.md)。

续跑不回滚工作区，也不保存可执行文件或脚本的内容快照。外部修改可能使历史上下文过时；文件写入仍执行现有的身份与修改时间检查。配置绑定保护的是本次明确记录的执行设置。

构建反馈回注前会对完整提案 `output`（去掉 `digest`）重算 SHA256，并核对保存的提案及决定摘要。检查不仅比较摘要字段，也验证其覆盖的内容。若新增的执行反馈需要按本次解析的密钥或既有 `CODESTUDIO_API_KEY` 脱敏，续跑在 MCP 启动及模型请求之前拒绝，不修改反馈后继续。

存在 `unknown` 或 `partial` 效果时拒绝续跑。`recover` 将已派发而没有结果的动作标为 `unknown`，不能推断它没有发生。已准备而尚未派发的动作记为 `not_executed`；续跑将该失败回执交给模型，不重新执行该调用。尚未准备的待处理调用可在重新通过权限检查后执行。

状态库只接受 schema 7；检查点封装、绑定和内核状态只接受 version 2。其他版本的读取和续跑均明确拒绝，不执行迁移，也不按任务过滤旧记录。请使用新的 `--state` 路径或新工作区创建任务；原数据库文件保持原样。没有检查点的任务不能续跑。

## 保存和失败边界

请求模型前保存尝试次数；完整模型回复校验后，保存待处理工具；每个工具结果或拒绝记录与检查点位置在同一事务中提交。进程在工具执行与结果提交之间崩溃时，效果仍按 `unknown` 处理，不会自动重试。

已知 API Key 会从检查点脱敏。如果脱敏改变了对话、调用 ID、参数、原生协议内容或配置，检查点会标为不可续跑，避免使用修改后的上下文。数据库里的其他对话和文件内容仍为明文，未实现加密存储。

也可在 `build-approve` 或 `build-reject` 中加入 `--continue-task` 和原任务全部授权，或在 RPC 中提供 `continue_options`，在同一次前台操作中继续原任务。只有该任务仍精确等待本次提案时才启动续跑；任务已推进后重发决定、当前请求取消或原批准执行已取消不会通过合并操作触发继续。回注与检查点推进在同一事务中记录 `build.proposal.consumed`，防止重复插入工具结果。合并操作保留原累计轮次预算并拒绝改变预算；要提高预算，另行使用上述显式 `resume --max-turns`。审批决定和独立构建先于续跑完成，后续校验失败不撤销决定、执行回执或已发布产物；合并返回保留这些结果，并用 `continuation.status: failed` 报告续跑失败。

## 后台登记与接管

`schedule-resume <task-id>` 可在登记时再次显式提供原授权，把已有暂停任务交给独立 worker。它保存精确的绑定、检查点 revision 和原累计预算，支持延迟、期限、有限尝试和安全模型请求退避。登记后仍需 `scheduler-start` 启动 worker；任务若等待构建决定，worker 等待明确审批和执行回执，每个新提案都再次暂停。完整命令和限制见 [后台续跑](scheduler.md)。

成功手动 `resume` 会在同一事务中取消该任务原有的排队登记；要再次后台处理，需在任务暂停后重新登记。正在执行的调度任务仍由 runner 锁保护，手动续跑不能并发接管它。调度恢复只核对自己遗留的任务并复用持久回执，其他 running 任务仍需显式 `recover`。

`cron-create` 另行登记周期新任务模板：每次触发从原始提示词和完整预算开始，不继承上一轮会话。一次 cron 任务内部的自动重试和显式续跑仍使用本文的累计预算、回执和未知效果规则；逐次生成新任务不能绕过阻断或重复审批。上一轮尚在运行、排队或等待审批时不生成重叠任务，手动接管同样受此约束。详见 [周期任务](scheduler.md#cron-周期任务)。

worker 每次续跑按保存引用读取凭据，系统库轮换在下一次操作生效；环境引用仍受启动环境限制，更新外部终端或服务环境不会影响已有 worker，需要从新环境重新启动。停止会取消当前活动 job，保留其他队列。Windows 可显式启用当前用户登录自启，Linux 使用 systemd 用户管理器启动；登记不立即执行或保存密钥，实际会话必须能读取任务绑定的环境/系统凭据。Windows/Linux 已有显式有界 worker 崩溃监督；人工核对后解除 unknown、权限变更迁移与上下文压缩仍待实现。Windows 子进程可选择 LPAC，构建与动态验证结果见 [验证记录](validation.md)。`completed` 仍只表示模型循环结束，不代表独立验收通过。
