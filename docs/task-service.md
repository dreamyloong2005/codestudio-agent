# 本地任务服务（协议 v2）

桌面客户端可作为父进程启动 `codestudio-agent serve --workspace <已有目录> [--state <数据库>]`，通过独占 stdin/stdout 管道交换 UTF-8、每行一个 JSON-RPC 2.0 消息。服务不监听 TCP 端口，stdout 只包含协议帧；启动错误写 stderr。客户端需要同时排空 stdout 和 stderr。

这是 CodeStudio Agent 向 CodeStudio Next 等可信客户端提供的接入协议；桌面 UI 的实现和技术选型属于独立的 CodeStudio Next 项目。服务与普通 CLI 共用 `execute_command`，复用进程授权、MCP、平台沙箱、SQLite 账本、工作区锁及恢复逻辑。持有管道的客户端被视为有权代表用户授权工具；该协议不是给不可信网页或插件开放的授权入口。

可运行的父进程接入示例见 [task_client.py](../examples/task_client.py)。默认只查列表；显式 `--demo` 会执行离线写入/读回，等待操作完成通知和持久化完成事件，再查询任务详情并关闭服务。

机器可读定义见 [task-service.schema.json](../protocol/task-service.schema.json)。该 JSON Schema 描述请求、响应和通知；`x-methods` 关联 35 个方法的参数、返回值及重试分类。它是客户端契约，宿主仍执行配置、授权和状态检查。现行协议仍为 2，新增能力通过初始化结果发现。`capabilities.mcp_streamable_http` 声明远程 MCP 客户端支持；任务仍通过 `mcp_config` 和 `allow_mcp` 显式授权，服务本身不新增 HTTP 监听端口。

## 初始化和查询

请求必须带 `jsonrpc: "2.0"`、`id` 和 `method`，`params` 缺省为对象。ID 支持有符号 64 位整数或 1–128 字节字符串。第一条有效请求应为：

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocol_version":2}}
```

返回协议版本、固定的工作区/数据库路径、能力及限制，以及本次服务进程唯一的 `session_id`。`capabilities.operation_lookup` 和 `structured_errors` 分别声明操作回执查询与结构化错误支持。重启服务会更换 session，不表示数据库或任务被重建。批量请求、重复 JSON 键、未知字段、错误类型和非当前协议版本均拒绝。客户端无 ID 通知不执行任何操作；所有改变状态的操作都需要请求 ID。

| 方法 | 参数 | 返回值 |
|---|---|---|
| `tasks/list` | `before` 默认 0，`limit` 默认 50 | `items`、`next_before`、`has_more`，按创建顺序倒序 |
| `tasks/get` | `task_id` | 元数据、`action_count` 和检查点摘要 |
| `events/list` | `after` 默认 0，`limit` 默认 50 | `items`、`next_after`、`has_more`，按事件 seq 升序 |
| `events/subscribe` | `after` 默认 0 | 确认游标，随后接收 `events/batch` 通知 |
| `events/unsubscribe` | 空对象 | 停止后续事件查询；已排队帧仍可能到达 |
| `operations/status` | 空对象 | `session_id`、`active` 与当前或最近的 `operation_id`；从未启动操作时 ID 为空字符串 |
| `operations/get` | `session_id`、`operation_id` | 当前操作或本会话保留的完成回执，查询不执行或续跑任务 |

查询使用独立的只读 SQLite 连接，不创建任务或迁移数据库。数据库不存在或新状态库仍在初始化时返回空页，后续会重新打开。已初始化状态库只接受 schema 7；schema 6 及其他不匹配版本在整个数据库入口拒绝，不按任务隐藏记录。需要继续工作时，使用新的 `--state` 路径或新工作区创建任务，原数据库文件不被转换。记录中的 `workspace` 表示其原工作区。

分页最多返回 100 条、序列化后不超过 2 MiB。客户端根据 `has_more` 和返回的 `next_before` / `next_after` 继续分页；游标对应实际已返回的任务或事件记录。

`prompt`/`summary` 最大 64 KiB，其他展示文本最大 8 KiB，事件 payload 最大 256 KiB。超大字段返回 `null`、`<字段>_omitted: true` 和 `<字段>_bytes`，不跳过事件或伪造完整内容；完整记录可通过 CLI `show` 查看。检查点只返回阶段、可恢复标志及轮次等摘要，不返回原始 state/binding；超过 16 MiB 的检查点内容会明确省略。检查点封装、绑定和内核状态只接受 version 2。

## 发起、取消和续跑

运行离线演示（会写入工作区的 `CODESTUDIO_DEMO.txt`）：

```json
{"jsonrpc":"2.0","id":2,"method":"tasks/start","params":{"mode":"demo","options":{}}}
```

真实模型任务使用 `mode: "run"`。`options` 支持以下结构化字段，语义与 CLI 对应：

```json
{
  "prompt": "阅读 README，说明构建步骤",
  "base_url": "https://your-provider.example/v1",
  "model": "your-model",
  "api": "responses",
  "credential": "system:work-model",
  "stream": true,
  "max_turns": 8,
  "timeout_ms": 60000,
  "process_timeout_ms": 10000,
  "max_output_tokens": 4096,
  "allow_write": false,
  "allow_build_proposals": false,
  "allow_http_loopback": false,
  "allow_process": [],
  "allow_mcp": [],
  "sandbox": "none"
}
```

可选 `mcp_config` 指定 version 2 的本地 MCP 配置清单，配合 `allow_mcp` 显式授权；该配置版本与 MCP 日期协议版本独立。`allow_process` 使用绝对 executable 路径。未知选项及 RPC 中的 `workspace`、`state`、`api_key` 均拒绝。`credential` 接受 `env:VARIABLE` 或 `system:identifier`，默认 `env:CODESTUDIO_API_KEY`；每次操作开始时读取、在操作期间固定使用，值不进入请求或绑定。系统凭据在调用服务前通过 CLI 配置，服务没有设置、删除或读取密钥值的 RPC，方法总数仍为 35。接口和平台条件见 [模型凭据与轮换](credentials.md)。`demo` 的 options 必须为空，其 provider 仍是固定脚本，不调用模型。

隔离工作区构建使用 `mode: "build"`，只接受一个显式构建清单：

```json
{"jsonrpc":"2.0","id":3,"method":"tasks/start","params":{"mode":"build","options":{"build_config":"D:/YourProject/build-policy.json"}}}
```

该模式不调用模型，经同一任务循环记录工具意图、派发与回执；清单固定构建输入、命令和输出规则。它不接受 `allow_process`、`allow_write`、网络、MCP、模型或 `sandbox` 覆盖，构建后端始终要求 OS 隔离且无未隔离降级。编译能力声明不代表运行环境满足隔离要求，也不代表任意工具链已通过运行验证。

`mode: "run"` 也可添加 `options.build_config`，向模型显式开放 `build_workspace` 工具。工具参数必须为空对象，模型不能在调用时改写冻结清单。该构建授权不改变其他已授权工具的运行方式；例如普通 `run_process` 仍遵循原 `sandbox` 选项。构建清单路径加入宿主文件工具的受保护路径。

启动和续跑立即返回 `{"session_id":"...","operation_id":"...","status":"accepted"}`，随后在工作线程执行完整预检；接受请求不代表任务已创建或工具已执行。`operation/finished` 通知包含同一 session/operation ID、`exit_code` 以及 `result` 或 `error`。成功创建的任务 ID 位于最终 result，也可从持久化 `task.started` 事件获取。服务每次只允许一个活动操作；同时启动或 recover 会报 busy。

取消使用 `operations/cancel`，参数为当前 `operation_id`。取消是请求，不代表操作已停；应等待 `operation/finished` 和账本结果。取消可能留下已发生或无法确认的工具效果，仍采用原有 `unknown` 语义。

`tasks/resume` 接受 `task_id` 和 `options`。需要重新提供原来的写入、进程、MCP、回环和沙箱授权，工作区/配置必须匹配检查点；不允许替换 prompt。省略 `credential` 时恢复保存引用，显式提供时必须完全相同；相同引用下的密钥可轮换。缺少该字段的旧模型检查点拒绝续跑，不补默认引用；数据库 schema 7 和协议/检查点 version 2 不变。省略 stream 会继承原值；显式 `false` 对应 `--no-stream`。首版拒绝续跑时显式 `stream: true`，避免 CLI 缺少强制开启选项时静默忽略请求。

原任务包含构建工具时，续跑必须再次提供同一 `build_config`，冻结构建绑定必须完全一致；绑定不匹配在 MCP 进程启动前拒绝。直接构建的 provider 为 `isolated-build`，续跑只接受 `build_config` 及可选 `max_turns`，不允许增加模型、进程、MCP、网络或沙箱授权。安全检查点可以复用已有回执，结果不明的构建不会自动重放。

`build_config` 只接受 v2 显式步骤清单，内部执行计划同样使用 version 2，不进行版本转换。构建绑定与提案回执使用 version 2，不包含插件标记。

原任务开放构建提案时，续跑必须再次提供 `allow_build_proposals: true`；该授权与检查点绑定，续跑不能新增或撤销。`waiting_approval` 任务需要对应决定和明确执行结果就绪后才能继续。续跑以原 tool call ID 回注决定和结果，保留原提案回执；回注与检查点推进原子记录 `build.proposal.consumed`，不重新执行构建。

`tasks/recover` 仅在空闲时运行原 CLI 恢复流程，返回 `recovered` 和 `tools_executed: false`。前台进程崩溃后的 running 状态必须先 recover，未知动作不能自动重放。持久后台授权需要另行请求 `schedules/create`；启动服务或恢复记录本身不会登记续跑、增加预算或启动 worker。

## 持久续跑队列

`initialize` 返回 `capabilities.background_scheduler: true` 和 `automatic_resume: true`。Windows/Linux 还返回 `worker_supervision: true`、`supervisor_diagnostics: true`，其他平台为 false，分别表示支持显式有界 worker 监督及其持久记录、事件查询和只读进程身份观测。客户端可通过 `schedules/create` 显式登记已有暂停任务的原授权，由独立 worker 按期限和原预算续跑；该接口不接受新 run 或 prompt。周期新任务使用下文的 `crons/create`。最多 100 个活动 job，同一任务最多一个，worker 每次运行一个任务。同一数据库只允许一个 worker，它只消费与启动工作区绑定匹配的 job；共享数据库的其他工作区需先停止当前 worker，再从对应工作区启动。

| 方法 | 严格接受的参数 | 同步返回值 |
|---|---|---|
| `schedules/create` | `task_id`，可选 `options`、`delay_ms`、`deadline_ms`、`max_attempts`、`retry_delay_ms` | 完整 job，`id` 为 job ID |
| `schedules/list` | 可选 `before`、`limit` | `items`、`next_before`、`has_more` |
| `schedules/get` | `job_id` | 完整 job |
| `schedules/cancel` | `job_id` | 更新后的 job |
| `scheduler/status` | 空对象 | 最后登记的 worker 状态及 `supervisor` 诊断对象 |
| `scheduler/supervisor/status` | 空对象 | `record`、`observation`、`worker_matches` |
| `scheduler/supervisor/events` | 可选 `after`、`limit` | 监督事件 `items`、`next_after`、`has_more` |
| `scheduler/start` | 可选 `poll_ms`、`supervise`；仅 `supervise: true` 时接受 `max_restarts`、`restart_delay_ms` | worker 状态及 `started: true`；监督时附带监督配置 |
| `scheduler/stop` | 空对象 | 状态及 `stop_requested: true` |

这些控制不占用前台 Operation 槽，不返回 operation ID。活动任务期间可以查询、取消或请求停止；登记仍受执行锁和续跑预检约束，不能绕过 busy。所有写请求必须有 ID，无 ID 通知不会修改队列或启动 worker。只读查询不创建数据库。

```json
{"jsonrpc":"2.0","id":20,"method":"schedules/create","params":{"task_id":"source-task-id","options":{"allow_build_proposals":true},"delay_ms":0,"deadline_ms":86400000,"max_attempts":8,"retry_delay_ms":1000}}
{"jsonrpc":"2.0","id":21,"method":"scheduler/start","params":{"poll_ms":100}}
{"jsonrpc":"2.0","id":22,"method":"schedules/get","params":{"job_id":"<create 返回的 id>"}}
```

`options` 复用 `tasks/resume`，需要重复全部原授权；不接受 `prompt`、`workspace`、`state`、`api_key` 或显式 `stream: true`。登记保存精确绑定、检查点 revision 和原累计轮次预算，不能增加预算。`delay_ms` 默认 0、范围 0..604800000；`deadline_ms` 默认 86400000、范围 1..604800000，且必须大于 delay。二者均相对登记时刻。`max_attempts` 默认 8、范围 1..100；`retry_delay_ms` 默认 1000、范围 100..60000；`poll_ms` 默认 100、范围 20..5000。分页 limit 默认 50、范围 1..100。

登记本身不启动 worker。Windows/Linux worker 成功独立启动后，客户端 EOF 和 `shutdown` 不会将它取消；Windows 不允许脱离的父 Job 会使后台启动明确失败，Linux worker 仍受原 cgroup、用户会话及 WSL 发行版生命周期约束。后台任务通过持久事件和队列/任务查询观察，不产生当前连接的 `operation/text` 或 `operation/finished`。每次续跑按保存引用读取凭据；系统库更新在下一次操作生效，当前操作不刷新。环境引用仍受 worker 启动环境限制，修改外部终端或服务不会改变已有进程，需从新环境重启。

`scheduler/start` 的 `supervise` 默认为 false。显式设为 true 后，独立监督进程在直接管理的 worker 异常退出后按有界策略重启。`max_restarts` 默认 3、范围 0..100，按监督进程整个生命周期累计，不因稳定运行而重置；`restart_delay_ms` 默认 1000、范围 100..60000，按次指数退避并封顶 60000 毫秒。未启用监督时提供这两个字段会拒绝请求。此模式不改变原任务的权限、预算、期限或未知动作恢复规则。

```json
{"jsonrpc":"2.0","id":23,"method":"scheduler/start","params":{"supervise":true,"poll_ms":100,"max_restarts":3,"restart_delay_ms":1000}}
```

监督启动成功时，在普通返回字段外增加 `supervised: true`、`supervisor_pid`、`max_restarts`、`restart_delay_ms`。`pid` 是已确认就绪的 worker，`supervisor_pid` 是监督进程；worker 重启后 PID 和 generation 可改变。启动成功只确认启动时刻，不保证以后一直运行。后续诊断由 `scheduler/status.supervisor` 或单独的 `scheduler/supervisor/status` 提供。`scheduler/stop` 同时阻止退避后的再次启动。正常退出、停止请求、所有权变化或重启次数耗尽后监督循环结束，监督器被强制结束时其子 worker 也被结束。原 stdio 连接不接收监督完成通知；完整前台回执、父子进程和凭据边界见 [有界 worker 监督](scheduler.md#有界-worker-监督)。

```json
{"jsonrpc":"2.0","id":24,"method":"scheduler/supervisor/status","params":{}}
{"jsonrpc":"2.0","id":25,"method":"scheduler/supervisor/events","params":{"after":0,"limit":50}}
```

监督诊断对象固定包含 `record`、`observation: {state, observed_at_ms}` 和 `worker_matches`。`record` 为该数据库最新监督运行的持久快照，包括运行 ID、owner、PID、平台出生身份、配置、`phase`、`restarts`、`last_exit_code`、`next_restart_at_ms`、退出原因和时间戳。阶段为 `starting`、`running`、`backoff`、`stopping`、`stopped`；尚无子 worker 退出回执时 `last_exit_code` 为 null，退避之外 `next_restart_at_ms` 为 0。`worker_matches` 仅比较同一数据库快照中 owner/generation 的关联，不证明 worker 存活。普通 worker 后续接管仍可能查到上一轮监督记录；没有监督记录时返回 `record: null`、`worker_matches: false`、`observation.state: not_checked`。

`observation.state` 另有四种取值：`matching_process` 表示查询时只读 OS 观测匹配保存的出生身份；`not_running` 表示进程已退出或 PID 不存在；`identity_mismatch` 表示 PID 已对应另一出生身份；`unavailable` 表示权限、平台或读取条件不足，无法可靠确认。观测不写入持久历史，也不把过时心跳当作退出证据。崩溃前保存的 `running` 可以与 `not_running` 同时出现；监督器保存 `stopped` 后尚未退出，也可以观测为 `matching_process`。数据库快照和 OS 观测并非跨系统原子事务，匹配不保证健康、工作进度或未来存活，更不授权按记录 PID 结束进程、重启或重放任务。

`scheduler/supervisor/events` 的 `after` 默认 0，范围为非负 64 位整数；`limit` 默认 50、范围 1..100。按 `seq` 递增返回严格大于 after 的已提交事件，每条包含 `seq`、`run_id`、`kind`、快照 `payload`、`created_at_ms`。阶段事件和 `supervisor.child_exited` 与快照原子保存，周期心跳不生成事件。客户端处理完成后保存 `next_after` 并继续读取；`has_more` 表示还有页。监督事件具有独立游标，不进入任务 `events/list` / `events/subscribe`，不产生当前连接推送；游标不能跨数据库替换或回滚使用。新运行取得准入锁后可将旧的非终态记录标记为 `stopped` / `superseded`，但不生成未观察到的子进程退出码。完整字段、事件种类及诊断解释见 [监督状态与事件](scheduler.md#监督状态与事件)。

成功手动 `tasks/resume` 会取消原队列登记。`schedules/cancel` 对运行中任务提交取消请求，需查询最终回执；`scheduler/stop` 取消活动 job 并保留其他队列。`scheduler/status.state`、PID 和心跳是最后登记信息，崩溃后可能过时，不能据此认定进程仍存活或直接结束记录的 PID。完整状态、审批与崩溃边界见 [后台续跑](scheduler.md)。

## 周期任务与登录自启

`initialize` 返回 `capabilities.cron: true`。`login_autostart` 和 `supervised_autostart` 在 Windows/Linux 为 true，其他平台为 false，分别声明自启后端和监督模式接入；它们是编译能力声明，实际登记仍受平台条件和当前用户权限约束。Windows 使用 Task Scheduler 登录触发；Linux 使用 systemd 用户管理器启动触发，返回 `user_manager_start`，不保证每次登录都会触发。

| 方法 | 严格接受的参数 | 返回方式 |
|---|---|---|
| `crons/create` | `expression`，可选 `name`、`timezone`、`options`、`deadline_ms`、`max_attempts`、`retry_delay_ms`、`max_runs` | 异步：先返回 operation ID，完成通知 result 为计划记录 |
| `crons/list` | 可选 `before`、`limit` | 同步：`items`、`next_before`、`has_more` |
| `crons/get` | `cron_id` | 同步：完整计划记录 |
| `crons/preview` | `expression`，可选 `timezone`、`after`、`count` | 同步：UTC/本地时间预览和触发策略 |
| `crons/pause` | `cron_id` | 同步：更新后的计划 |
| `crons/resume` | `cron_id` | 同步：更新后的计划 |
| `crons/cancel` | `cron_id` | 同步：更新后的计划；同时向活动 job 提交取消请求 |
| `scheduler/autostart/status` | 空对象 | 同步：当前用户的登记信息 |
| `scheduler/autostart/enable` | 可选 `poll_ms`、`supervise`；仅 `supervise: true` 时接受 `max_restarts`、`restart_delay_ms` | 同步：登记结果，`started: false` |
| `scheduler/autostart/disable` | 空对象 | 同步：移除匹配登记的结果 |

`crons/create.options` 复用 `tasks/start` 的 `run` 选项，包含新任务的 `prompt`、模型配置及显式工具授权；不接受 `workspace`、`state`、`api_key`。模板冻结精确配置、权限和工具 schema。创建不请求模型、不执行模型工具，也不启动 worker；若包含 MCP，会启动已授权服务器完成发现，因此采用可取消的前台 Operation，并遵循已有 busy、EOF 和 shutdown 规则。接受请求不等于登记已成功，客户端应等 `operation/finished` 并保存 result 中的计划 `id`。

```json
{"jsonrpc":"2.0","id":30,"method":"crons/create","params":{"name":"工作日检查","expression":"0 9 * * 1-5","timezone":"Asia/Shanghai","max_runs":20,"deadline_ms":3600000,"options":{"prompt":"阅读 README.md，报告需要关注的问题。","api":"responses","base_url":"https://your-provider.example/v1","model":"your-model","max_turns":8}}}
{"jsonrpc":"2.0","id":30,"result":{"session_id":"<initialize.session_id>","operation_id":"create-operation-id","status":"accepted"}}
{"jsonrpc":"2.0","method":"operation/finished","params":{"session_id":"<initialize.session_id>","operation_id":"create-operation-id","exit_code":0,"result":{"id":"cron-id","status":"active"}}}
{"jsonrpc":"2.0","id":31,"method":"crons/get","params":{"cron_id":"cron-id"}}
{"jsonrpc":"2.0","id":32,"method":"scheduler/autostart/enable","params":{"poll_ms":100}}
```

上例完成通知的 result 仅展示身份和状态字段，实际返回完整计划。计划包含 `expression`、`timezone`、`next_due_at_ms`、`last_due_at_ms`、`run_count`、`max_runs`、`last_job_id` 等信息。`crons/create` 完成之后的后台触发不属于该 operation，不向这条连接发送其模型文本或 operation 完成通知；通过持久事件和 task/job 查询观察。`crons/list|get` 不暴露初始检查点 state，授权参数和 binding 仍属于明文记录。

表达式为分钟、小时、日期、月份和星期，共五个字段，支持数字、`*`、列表、范围和步长。日期、星期同时受限制时用 OR；含 `*` 或 `*/1` 的日字段不限制另一日字段，`*/2` 等较大步长仍按受限制字段处理。`timezone` 默认 `local`，创建时固定为系统当前 IANA 名称；支持 UTC/IANA，夏令时缺失分钟跳过，回拨重复分钟分别对应两个 UTC 时刻。预览 `after` 是 UTC epoch 毫秒，默认当前时间，结果严格晚于它；`count` 默认 5，范围 1..100。语法和时区数据限制详见 [Cron 周期任务](scheduler.md#cron-周期任务)。

每个触发生成新 task/job 和完整独立的模型预算；`max_runs` 默认 0，范围 0..1000000，0 表示持续重复。期限、重试参数与续跑队列范围相同，但期限相对此次任务生成时间。错过的触发合并成最新一次；上一轮排队、运行或等待审批期间的触发跳过，不补回、不消耗 `max_runs`。前一轮出现阻断或未知效果时计划自动暂停；每轮构建提案仍单独审批。`pause` 不停止已有工作；`resume` 从现在以后的时刻继续；`cancel` 取消未来触发并取消活动 job，已取消或次数耗尽的计划不能恢复。

其他 cron 和自启控制不占用 Operation 槽，活动前台操作期间仍可查询、暂停或取消。所有写请求必须有 ID。`crons/preview` 不创建数据库或任务；自启登记独立于数据库创建，不读取或保存模型密钥。

自启默认 `supervise: false`，执行固定路径的 `scheduler-run`；显式 `supervise: true` 则登记 `scheduler-supervise`。`poll_ms` 默认 100、范围 20..5000；监督时 `max_restarts` 默认 3、范围 0..100，`restart_delay_ms` 默认 1000、范围 100..60000。未启用监督时提供任一监督限制会拒绝。模式、轮询间隔和监督限制都固定保存；同一用户、工作区和数据库只对应一份登记，不能直接覆盖不同配置。切换时先 disable，再 enable：

```json
{"jsonrpc":"2.0","id":33,"method":"scheduler/autostart/disable","params":{}}
{"jsonrpc":"2.0","id":34,"method":"scheduler/autostart/enable","params":{"supervise":true,"poll_ms":100,"max_restarts":3,"restart_delay_ms":1000}}
{"jsonrpc":"2.0","id":35,"method":"scheduler/autostart/status","params":{}}
```

应等待 disable 成功后再发送 enable。自启返回值始终包含 `supervised`；只有为 true 时包含 `max_restarts`、`restart_delay_ms`。`status` 和 `disable` 从完整核验的已保存登记推断实际模式，无需客户端重复提供限制；disable 回执保留被移除登记的配置。没有登记或 `conflict: true` 时，返回的默认/请求选项不能视为已存在配置。损坏或外来登记即使名称相同也不能覆盖或删除。

Windows Task Scheduler 绑定当前用户的交互登录令牌，普通权限、无保存密码。`enable` 不立即启动进程，`disable` 不停止当前 worker 或监督器；立即启动或停止仍用 `scheduler/start|stop`，启动监督器须再次显式选择 `supervise: true`。`Hidden` 仅隐藏计划任务登记项，不保证控制台进程没有窗口。系统 `restart_count` 固定为 0，监督器自身不会在退出后被自动拉起；后续登录触发的新监督生命周期重新计算 worker 重启次数，但不重置任务原授权、期限或预算。环境凭据来自实际登录启动环境，不能依赖开启登记时某个终端的临时变量；系统引用由任务开始时访问当前用户凭据库，实际会话中仍须可读取。完整命令与生命周期见 [登录自启](scheduler.md#windows-当前用户登录自启)。

Linux 写入当前用户专属的 systemd 单元和启用链接，核验准确归属后才允许修改或移除。普通与监督模式均保留 `Restart=no`，不由 systemd 重启 worker 或监督器；之后的新用户管理器启动触发可开启新的监督生命周期。登记不 reload/start/stop 用户管理器，不调整 linger；文件变更返回 `requires_manager_reload: true`，默认由下次用户管理器启动采用。环境凭据来自该管理器的实际环境；系统引用要求 worker 可连接用户的 Secret Service 且默认集合未锁定。WSL 发行版须先启动。完整条件与语义见 [后台调度](scheduler.md) 中的 Linux 自启说明。

## 构建提案与审批

`initialize` 的 `capabilities.build_proposals: true` 表示服务支持构建提案，`approval_continuation: true` 表示支持审批后显式续跑；`automatic_resume: true` 对应上面的独立持久队列。`tasks/start` 的 `mode: "run"` 可通过 `options.allow_build_proposals: true` 向模型开放 `propose_build` 工具。模型提交 `manifest` 与 `reason`；宿主仅校验 version 2 的显式步骤清单、规范化并冻结执行计划，不复制工作区、不启动构建。成功动作的 effect 为 `none`，回执状态为 `pending_approval`。源任务暂停为 `waiting_approval`，本次 operation 返回退出码 3，并在 result 中提供 `approval.action_id` 和 `approval.digest`；该工具调用暂不回复模型。

持有 stdio 管道的可信客户端负责向用户展示提案并取得授权。模型没有批准或拒绝工具；允许提出构建方案不等于允许执行。

| 方法 | 严格接受的参数 | 执行方式 |
|---|---|---|
| `builds/review` | `task_id`、`action_id` | 同步只读；活动操作期间也可查询 |
| `builds/approve` | `task_id`、`action_id`、`digest`、可选 `continue_options` | 异步 Operation；空闲时接受 |
| `builds/reject` | `task_id`、`action_id`、`digest`、可选 `continue_options` | 异步 Operation；空闲时接受 |

```json
{"jsonrpc":"2.0","id":10,"method":"builds/review","params":{"task_id":"source-task-id","action_id":"source-action-id"}}
```

查询返回 `task_id`、`action_id`、完整 `proposal` 和 `decision`。`proposal.version` 为 2，其余字段为 `status`、`workspace`、`reason`、`manifest`、`recipe`、`protected_paths`、`backend`、`network`、`export`、`digest`；其中 `recipe` 是将执行的 v2 冻结计划，`backend` 在 Windows 为 `windows-lpac-build-v2`，在 Linux 为 `linux-namespace-build-v2`。`digest` 是除去自身字段后，对本协议规范 JSON 编码计算的 SHA-256（64 位小写十六进制），具体编码为 nlohmann::json 的 UTF-8 `dump()`。客户端应展示命令、工具链、输入、保护路径、网络及导出范围，再原样将本次展示的 digest 用于审批；不可静默改用新查询所得摘要。摘要不锁定源码或编译器的历史字节，执行时读取当时的文件内容。

未决定时 `decision` 为 `null`；决定后包含 `decision: "approved" | "rejected"`、`digest`、`execution_task_id` 和 `execution_action_id`，拒绝时执行标识为空值。查询会验证原动作、摘要和工作区；提案不存在或被改动时返回错误，且不创建数据库。

```json
{"jsonrpc":"2.0","id":11,"method":"builds/approve","params":{"task_id":"source-task-id","action_id":"source-action-id","digest":"<review 返回的 64 位小写 SHA-256>"}}
```

拒绝使用相同参数、方法名改为 `builds/reject`。这两个方法先返回 `operation_id`，随后以 `operation/finished` 返回 `exit_code` 和 `result`；结果为 `{"proposal": <最新 review 对象>, "execution": <执行任务详情或 null>, "state_database": "..."}`。输入参数不接受 `workspace`、`state` 或 `options` 覆盖，始终使用服务启动时的固定工作区和数据库；无请求 ID 的通知不执行审批。

批准时，账本在同一事务中保存决定、创建独立的 `provider: "approved-build"` 任务及 prepared `build_workspace` 动作，随后通过原隔离构建执行路径派发。默认审批操作本身不调用模型，但批准执行前须能读取源任务保存的凭据引用，以对新执行回执脱敏；单独拒绝不要求密钥可用。批准并继续在同一操作中固定使用一次解析的值，`continue_options.credential` 若提供必须匹配源任务引用。若任务已单独登记，worker 在决定和执行结果就绪后按保存引用重新读取凭据并继续。重复批准只返回原执行任务；重复拒绝返回原决定。相反决定、错误摘要或跨工作区审批会失败。

批准任务不保存模型检查点，不能通过 `tasks/resume` 自动续跑。崩溃后先调用 `tasks/recover`：已派发而结果不明的动作保留 `unknown`；再次批准也只返回原任务，绝不自动重放。任务完成、取消和其他执行结果分别对应退出码 0、130 和 1，配置或授权错误为 2。审批请求仍需要遵循单活动 Operation 的约束。

需要在决定后继续原任务时，提供 `continue_options`：

```json
{"jsonrpc":"2.0","id":12,"method":"builds/approve","params":{"task_id":"source-task-id","action_id":"source-action-id","digest":"<review 返回的 64 位小写 SHA-256>","continue_options":{"allow_build_proposals":true}}}
```

拒绝后继续使用同样参数和 `builds/reject`。`continue_options` 必须为对象，字段、范围及类型校验完全复用 `tasks/resume.options`；它不接受 `workspace`、`state`、`api_key` 或 `prompt`，也不接受显式 `stream: true`。提供 `{}` 同样请求继续，但不会自动授予原权限；客户端仍须重复全部原授权。`builds/review` 不接受 `continue_options`。

回注前会对完整提案 `output` 去掉 `digest` 后重算 SHA256，并核对提案和决定摘要。新增执行反馈如果需要当前 `CODESTUDIO_API_KEY` 脱敏，会在 MCP 启动及模型请求前拒绝续跑。

合并操作只在源任务仍精确等待本次提案时启动续跑。任务已推进、重复决定不能匹配当前等待项、请求被取消或原批准执行已取消时跳过；待决、运行中的构建和未知效果不能继续。审批与续跑共享同一 operation ID，后续文本使用现有 `operation/text`；合并操作要求原累计轮次预算保持不变；提供不同 `max_turns` 会使续跑失败，需要提高预算时应另发 `tasks/resume`。

最终 `result` 保留决定结构并增加 `continuation`：`{"status":"started","result": <原任务结果>,"exit_code": <续跑退出码>}`、`{"status":"skipped","reason":"..."}` 或 `{"status":"failed","error": <错误信息>}`。`started` 只表示实际运行了续跑，不保证完成；原任务可能再次进入 `waiting_approval`。`started` 时顶层退出码沿用续跑退出码，`failed` 为 2，取消为 130；`skipped` 保留决定阶段退出码，取消除外。续跑错误不回滚已提交的决定、独立执行回执或已发布产物，最终返回仍保留决定阶段结果；顶层失败不表示构建未执行。调用方应检查两个阶段的结果。合并审批不会创建队列登记；每个新提案始终需要新的明确审批。

## 事件和文本

`events/batch` 参数与 `events/list` 返回值一致。每条记录含 `seq`、`task_id`、`kind`、`payload`、`created_at` 和 `schema_version: 2`。服务约每 50ms 读取一页已提交数据，保持 seq 递增；客户端用最后**实际处理完成**的 seq 重新订阅，可以重放漏读事件。游标只适用于同一数据库，不支持替换/回滚数据库后沿用旧游标。事件读取失败时发送 `events/error` 并停止订阅，客户端可检查错误后显式重新订阅。

`operation/text` 含 `operation_id`、`text` 和 `provisional: true`。它携带实时模型文本，保留 UTF-8/换行，跨分片过滤当前配置的密钥；客户端应按纯文本渲染。文本增量没有持久游标，不保证断连重放，也不能作为工具执行或任务完成的证据。持久化通知和操作完成通知来自不同路径，不承诺二者之间的总顺序；同一订阅的事件按 seq 排序。

请求 ID 用于响应关联，不是幂等键。客户端不能因没收到结果就重发启动/工具请求，应先查询数据库和 operation 状态；operation ID 只在本次服务进程内有效。

## 操作回执和断连处理

`operations/get` 要求显式提交初始化返回的 session ID，防止客户端把旧进程的操作误当作新进程状态：

```json
{"jsonrpc":"2.0","id":40,"method":"operations/get","params":{"session_id":"<initialize.session_id>","operation_id":"<accepted.operation_id>"}}
```

结果包含 `session_id`、`operation_id`、原始 `method`、`state`、`cancel_requested` 和 `completion`。运行中 `state` 为 `running`、`completion` 为 null；完成后为 `finished`，completion 与 `operation/finished` 参数一致。finished 只表示本次操作结束，任务可能失败、取消或等待审批；应检查退出码、结果和持久记录。取消标志表示服务收到了请求，不证明取消抢在副作用之前发生。

服务最多保留最近 64 次完成操作，外加当前活动操作，限制由 `limits.operation_history` 声明。每份完成回执中的 result 最多保留 256 KiB，由 `limits.operation_result_bytes` 声明；超限时 result 为 null，并返回 `result_omitted: true`、`result_bytes` 和 `result_refs`。refs 只保留原 result 顶层长度符合限制的 `task_id` / `id`，其中 cron 创建结果的 id 是 cron ID；不能假定一定有引用。查询持久任务、计划或事件以获取后续状态。服务不单独缓存请求参数或临时文本流；完成结果可能包含原有持久记录中的提示词与授权信息，例如 cron 计划。回执内容与输出一样先经过凭据脱敏。

回执查询规则：

1. 同一服务仍可通信时，用已确认的 session/operation 查询；查询不会再次执行动作，也不会重新发送文本流。
2. stdio 关闭后，前台服务取消活动操作并退出。新的父子连接是新的服务 session。使用任务 ID 查询持久记录，并从最后实际处理的事件 seq 重放；硬崩溃后的 running 任务按原规则显式 recover。
3. 若丢失 accepted 或任务 ID，查询任务列表及持久事件核对。无法唯一确认原请求时，交由调用方处理，不能自动重复提交写请求。
4. `session_mismatch` 或 `operation_not_found` 只说明缓存不可用。即使任务已经完成并产生文件，也可能得到这类错误；它们不证明动作未执行。

数据库路径替换/回滚后不能继续使用原事件游标。操作查询不提供跨重启幂等键，也不改变原任务恢复、审批唯一性和未知效果阻断规则。

## 结构化错误

RPC 错误保留 JSON-RPC 的 `code` 和可读 `message`，新增 `data.kind`、`recovery`、`outcome` 和 `session_id`。异步操作失败的 `completion.error` 使用同一结构；`events/error` 保留 message/after，并附带 `error` 对象。客户端按 kind 分支，不解析英文错误文本。错误消息先脱敏、再按完整 UTF-8 字符截到最多 8 KiB；截断时 `data.message_truncated` 为 true。

| kind | recovery | 含义 |
|---|---|---|
| `parse_error` / `invalid_request` / `invalid_params` / `method_not_found` | `fix_request` | 修正请求；未进入该请求的执行路径 |
| `not_initialized` | `initialize` | 先完成初始化 |
| `already_initialized` | `none` | 使用已有初始化结果 |
| `busy` | `wait_for_idle` | 前台操作仍活动；等候或明确取消后再决定是否提交 |
| `session_mismatch` / 查询时的 `operation_not_found` | `query_state` | 改查持久任务与事件；原操作结果不能由缓存缺失推断 |
| 取消时的 `operation_not_found` | `query_operation` | 本次取消未接受；核对原操作与任务 |
| `command_failed` / `operation_failed` / `internal_error` | `query_state` | 核对已提交状态和回执，禁止基于错误码自动重放 |

`outcome: not_started` 仅描述这次被拒请求的执行路径，不描述过去的任务；`unknown` 表示错误层无法证明没有副作用。通用命令异常和异步异常保守返回 unknown，线程无法创建则返回 not_started。它们不是持久工具回执的 effect 值，也不是新增的重试授权。业务命令正常返回的 result（含失败任务或审批 continuation）仍遵循各自原有结构。服务无法启动或管道已损坏时，可能没有可交付的结构化帧。

## 生命周期与限制

`shutdown`（空参数）返回 `stopping: true`，请求取消活动前台操作，然后等待执行线程和输出完成。stdin EOF 或控制台中断同样取消；stdout 断开在下一次写入失败时被发现。硬结束前台进程仍需下次显式 recover。同步系统调用（例如 profile 准备）不能立即中断。已经成功独立启动的后台 worker 继续存在；停止它需要单独的 `scheduler/stop`。

输入帧最多 1 MiB，输出帧最多 4 MiB，排队输出最多 16 MiB；单次写入超过约 5 秒会停止连接并取消活动操作，防止不读管道的客户端无限占用资源。超长输入在报告错误后关闭服务。输出超限/断连可能导致最终通知无法送达，结果以数据库为准。客户端关闭时应持续读取输出直至进程退出。

服务尚不包含 HTTP/WebSocket、并行任务执行或跨用户认证。独立 worker 需要显式启动，或由已明确登记的平台自启触发；它不是未登录系统服务。`scheduler/start` 支持可脱离父 Job 的 Windows 环境及 Linux 独立后台进程，并可显式启用有界监督；自启也可显式登记监督模式，默认仍运行普通 worker，监督器本身不自动重启。实际运行和登记验证以 [验证记录](validation.md) 为准；macOS 尚不支持。桌面展示与权限交互由 CodeStudio Next 等可信客户端负责，Agent 校验收到的授权并执行相同策略。

## OAuth 与插件管理

新增 `mcp/auth/discover|login|status|logout` 和 `plugins/pack|install|lock|inspect`
异步操作。登录通过 `operation/authorization` 提供授权 URL，客户端负责展示并
由用户完成授权；不向任务服务发送 code、PKCE verifier 或令牌。插件管理与 OAuth
连接均不隐式授予工具权限。参数见[机器契约](../protocol/task-service.schema.json)，
完整流程见 [OAuth](mcp-oauth.md) 和[插件分发](plugins.md)。

路径相对于服务固定工作区解析。操作查询与取消沿用现有会话规则；已完成的令牌
交换、发布或换锁不会因取消而回滚。断连后先查询当前操作或显式检查连接/插件状态。

`capabilities.plugin_sandbox` 声明编译的完整插件包隔离支持，不代表当前主机运行探测成功。通过已有 `options.sandbox` 与 MCP 清单中的 `sandbox_limits` 使用；未新增运行方法或自动工具授权，详见[插件隔离](plugin-sandbox.md)。

## 隔离资源管理

`sandboxes/list` 接受可选 `after` 资源 ID 与 1–100 的 `limit`，同步返回当前用户登记目录的分页观察，可在前台操作活动期间调用。`sandboxes/reclaim` 接受必填 `id` 与可选布尔 `dry_run`，按已有异步操作协议返回并支持取消。输入不能指定任意待删除路径；不带请求 ID 的通知无回收效果。

`capabilities.sandbox_resources` 仅表示编译支持。接口覆盖当前用户登记资源，不局限于服务工作区，属于可信客户端管理能力，不向模型暴露。回收失败或取消保留登记供后续检查，断连后先查询当前资源状态。它不修改任务数据库，也不改变未知工具效果。状态字段与平台限制见[隔离资源登记与回收](sandbox-resources.md)。
