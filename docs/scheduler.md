# 后台调度、Cron 与登录自启

调度器将任务授权保存到 SQLite，由独立 worker 在到期、重试或审批结果就绪时执行。`schedule-resume` 登记已有任务，保留同一会话和累计预算；`cron-create` 登记周期模板，每次触发创建独立任务和预算。手动 `resume`、调度执行和本地任务服务共用权限、检查点、工具回执与工作区执行锁。

状态库只接受 **schema 7**；检查点封装、执行绑定、内核状态、本地 MCP 配置、任务服务协议、任务事件和产物清单仍使用 **version 2**。已初始化的 schema 6 数据库及其他不匹配版本明确拒绝，不迁移、转换或删除旧数据。请指定新的 `--state` 路径或新工作区，并创建新任务。

## 排队和启动

原任务暂停后，显式重复其全部授权。例如，原任务只开放构建提案时：

```powershell
codestudio-agent schedule-resume <task-id> --workspace D:\YourProject `
  --allow-build-proposals --delay-ms 0 --deadline-ms 86400000 `
  --max-attempts 8 --retry-delay-ms 1000

codestudio-agent scheduler-start --workspace D:\YourProject
codestudio-agent schedule-list --workspace D:\YourProject
codestudio-agent schedule-show <job-id> --workspace D:\YourProject
```

`schedule-resume` 返回持久化队列记录，其中 `id` 是 job ID，`task_id` 是原任务 ID。登记本身不请求模型、不启动 MCP，也不自动启动 worker。使用自定义数据库时，每条命令都要提供同一 `--state <数据库>`。

原任务使用过的 `--allow-write`、`--allow-process`、`--allow-mcp`、`--mcp-config`、`--build-config`、`--allow-build-proposals`、`--allow-http-loopback` 和 `--sandbox` 必须再次提供。模型、端点和 `credential` 引用默认从检查点恢复，显式覆盖必须与原配置一致。登记保存这次明确授予的权限、精确的执行绑定、检查点 revision 和原累计模型轮次预算；后续执行仍重新核验它们。缺少凭据引用的旧模型检查点拒绝登记，不补默认值或转换。

可登记的任务状态为 `interrupted`、`cancelled`、`failed`、`exhausted` 或 `waiting_approval`，且必须有安全、可续跑的当前版本检查点。模型轮次预算必须尚有余额；处于工具阶段时可以先消费已经保存的调用或回执。调度不能提高原 `max_turns`，也不能增加权限；需要改预算时先显式手动续跑。已完成任务和没有会话检查点的独立审批构建任务不能登记。

同一任务最多一个活动 job，同一数据库最多 **100 个活动 job**。同一数据库同时只允许一个 worker；worker 启动时固定工作区，只执行绑定匹配该工作区的 job。多个工作区共用一个数据库时，需要先停止当前 worker，再从目标工作区启动，才能处理该工作区的队列。worker 一次执行一个 job；任务执行期间获取工作区和数据库 runner 锁，空闲等待时不占用它们。其他前台任务占用执行锁时，worker 等待后再检查，不消耗一次执行尝试。

## 延迟、重试和状态

| 选项 | 默认 | 范围与含义 |
|---|---:|---|
| `--delay-ms` | 0 | 0..604800000，距登记时间的首次执行延迟 |
| `--deadline-ms` | 86400000 | 1..604800000，距登记时间的总期限，必须大于 delay |
| `--max-attempts` | 8 | 1..100，最多领取执行次数 |
| `--retry-delay-ms` | 1000 | 100..60000，模型请求安全失败后的初始退避 |
| `--poll-ms` | 100 | 20..5000，`scheduler-start` / `scheduler-run` / `scheduler-supervise` 检查间隔 |
| `schedule-list --limit` | 50 | 1..100，结合 `--before` 分页 |

延迟和期限在登记时换算为绝对时间 `due_at_ms` / `deadline_at_ms`。总期限包含排队、等待审批、执行及退避，不会因重启而重新计时。重试延迟随尝试次数倍增，上限 60000ms；每次实际领取执行增加 `attempts`，空轮询和审批等待不增加它。

领取和正式续跑前严格检查截止时间。已经运行的模型或工具通过轮询发出协作取消，响应时间受 `--poll-ms` 和执行收尾影响；这不是操作系统强制的硬截止。

只有停在 `model_pending`、检查点可续跑、没有不明效果且仍有原预算的模型请求失败可以自动重试。工具失败、配置变化、不兼容检查点或结果不明的动作不会通过调度重放。重发中断的模型请求会继续消耗原任务预算，也可能再次产生模型费用。

| job 状态 | 含义 |
|---|---|
| `queued` | 等待到期或安全重试 |
| `claimed` / `running` | worker 已领取 / 正在续跑 |
| `waiting_approval` | 等待当前提案的明确决定及必要执行回执 |
| `completed` | 原任务正常结束；仍不代表独立验收通过 |
| `blocked` | 检查点、授权、工具效果或其他条件阻止自动继续 |
| `cancelled` | 此次登记已取消或被手动续跑取代 |
| `expired` | 总期限已到，或下一次重试已超出期限 |
| `exhausted` | 尝试次数或原模型预算耗尽 |

查询同时返回 `last_error`、`attempts`、`cancel_requested`、时间字段和保存的授权信息。队列与对话一样是明文数据；不要把凭据放进命令参数或配置内容。参数最多 256 项、序列化后 64 KiB，执行绑定最多 256 KiB；需要按当前配置密钥脱敏才能保存的授权会被拒绝。

## 审批和手动接管

等待审批的任务可以先登记，再按 [构建提案流程](build-proposals.md) 使用本次审阅的精确摘要批准或拒绝。默认审批操作本身不请求模型；已单独授权并启动的 worker 会在决定和明确的执行结果就绪后继续原任务，将结果回注原 tool call ID。

若模型又提交新提案，任务和 job 再次进入 `waiting_approval`。持久续跑授权不包含批准新构建的权限，每个提案都需要新的明确决定。批准执行仍在运行、效果未知或提案内容被改动时不能继续。审批等待也受原登记期限约束。

成功的手动 `resume`（包括审批时显式请求的续跑）会在同一事务中取消该任务现有的排队登记，并记录它已被手动操作取代。它不会与正在运行的调度任务并发执行；runner 锁仍防止双重执行。手动续跑后若还需后台处理，应在任务再次暂停时重新登记。

## 取消、停止和崩溃恢复

```powershell
# 取消一个 job；运行中时是持久化取消请求
codestudio-agent schedule-cancel <job-id> --workspace D:\YourProject

# 请求 worker 停止，同时取消其当前活动 job，保留其余队列
codestudio-agent scheduler-stop --workspace D:\YourProject
codestudio-agent scheduler-status --workspace D:\YourProject
```

取消不是回滚：工具可能已经产生效果，最终判断仍以动作回执为准。运行中的取消请求需要等待执行收尾；不要将 `cancel_requested: true` 当作已经退出。停止 worker 不清空其他排队任务，重新启动后仍按各自原期限处理。

`scheduler-status` 返回 worker 的 `state`、`owner`、`pid`、`generation`、`stop_epoch`、`started_at_ms` 和 `heartbeat_at_ms`，并增加下文的 `supervisor` 诊断对象。worker 字段是**最后登记的状态**，崩溃后 `state: running` 或 PID 可能已过时；心跳与 PID 都不是进程仍存活的证明。`state: stopped` 也可能只是停止请求已提交，当前任务尚在收尾。控制应通过停止/取消命令进行，不要依据保存的 PID 直接结束进程。

新 worker 获取独占调度锁后，会在任务执行锁保护下核对前一 worker 遗留的 `claimed` / `running` job。已派发但无回执的动作成为 `unknown`，已准备但未派发的动作成为 `not_executed`。检查点、绑定、预算和已知回执仍安全时可以重新排队；未知效果或状态冲突会进入 `blocked`，不重放工具。已完成的工具结果与检查点推进原子保存，恢复时复用这些回执。

该恢复只处理调度器拥有的任务。其他前台进程崩溃遗留的 running 任务仍需显式 `recover`；worker 不会自动接管未登记任务。人工解除 unknown、迁移授权或修复旧版本状态仍未提供。

## 进程与凭据生命周期

Windows 的 `scheduler-start` 启动隐藏的独立 worker，标准句柄指向 NUL，不继承客户端管道或 runner 锁。启动器在恢复子进程前确认它已脱离父进程 Job；无法获得独立生命周期时明确失败。嵌套 Job 中仅允许脱离内层也不足以启动，外层仍有成员关系就拒绝。遇到此限制，可从不受该限制的独立终端启动；不会借助系统 broker 绕过限制。stdio 客户端 EOF、`shutdown` 或客户端自身退出不会停止已成功启动的 worker。

Linux 的 `scheduler-start` 通过 `setsid` 和双重 fork 脱离启动终端，三个标准描述符指向 `/dev/null`，执行前关闭其他继承描述符，包括客户端管道和 runner 锁。启动器通过专用管道确认 `exec` 成功，再等待本次 owner/PID 登记；客户端 EOF 或退出不会停止已经启动的 worker。此方式不脱离已有 cgroup，也不改变系统的会话清理策略；注销、用户管理器停止或 WSL 发行版关闭仍可能结束它。

启动器等待最多约 5 秒确认本次 owner/PID 已登记。若提示启动未确认，应先检查状态和队列再决定重试；这个错误不能证明 worker 已退出。普通独立 worker 不自动拉起；需要时显式启用下文的有界监督。Windows 与 Linux 可另行显式登记下文的平台自启；登记和当前会话中的 worker 启动是两个独立操作。

任务绑定保存 `env:VARIABLE` 或 `system:identifier` 凭据引用，密钥值不保存进队列、绑定或命令行。默认引用为 `env:CODESTUDIO_API_KEY`。worker 每次开始续跑时解析保存的引用，当前任务固定使用取得的值；更新同一个系统凭据后，后续任务可直接读取新值，无需重启 worker。缺失、锁定或不可用时失败，不回退环境密钥，也不因认证失败隐式刷新或重放。详见 [模型凭据与轮换](credentials.md)。

环境引用仍读取 **worker 启动时继承的环境**。更新其他终端或 stdio 服务的环境不会改变已运行 worker；这种来源的轮换需要停止旧 worker，再从新环境重新启动。停止会取消其活动 job，若需继续该任务，要检查回执并重新明确登记。通过旧服务进程重新启动 worker 仍会继承旧服务的环境。

也可在前台运行同一个循环：

```powershell
codestudio-agent scheduler-run --workspace D:\YourProject --poll-ms 100
# 仅执行一次扫描/执行周期后退出
codestudio-agent scheduler-run --workspace D:\YourProject --once
```

Linux 命令语义相同，使用本机路径和 shell 续行语法，例如 `codestudio-agent scheduler-start --workspace /home/user/project`。Linux 编译与运行验证范围见 [验证记录](validation.md)；macOS 等其他平台的后台启动和自启仍明确不支持。

## 有界 worker 监督

Windows/Linux 可显式启动监督进程，在其直接管理的 worker 异常退出后重启：

```powershell
# 当前终端运行监督进程；子 worker 使用独立进程执行。
codestudio-agent scheduler-supervise --workspace D:\YourProject `
  --max-restarts 3 --restart-delay-ms 1000

# 独立后台启动监督进程，客户端退出后继续运行。
codestudio-agent scheduler-start --workspace D:\YourProject --supervise `
  --max-restarts 3 --restart-delay-ms 1000

# 停止请求在运行期和重启退避期均生效。
codestudio-agent scheduler-stop --workspace D:\YourProject
```

`--max-restarts` 默认 3、范围 0..100，表示第一次启动之后允许的重启总次数；0 表示只运行一次。次数贯穿该监督进程的整个生命周期，worker 长时间正常运行不会重置计数。`--restart-delay-ms` 默认 1000、范围 100..60000；第一次重启等待该时长，后续按两倍递增，最多 60000 毫秒。两项限制接受于 `scheduler-supervise`、带 `--supervise` 的 `scheduler-start` 或 `scheduler-autostart-enable`；普通模式和 `scheduler-run` 不接受它们。监督模式不接受 `--once`。

监督器持有工作区和状态库的独占准入锁，重启退避期间仍阻止第二个监督器或普通 worker 接管。它保留直接创建的子进程身份，确认该子进程退出后才考虑下一次启动；不会根据心跳变旧去杀进程，也不重新打开状态记录中的 PID。每次子 worker 登记都会原子核对停止代数和上一 worker 的 generation，停止请求或所有权变化不能被重启覆盖。

正常退出不会重启。子 worker 尚未成功登记就退出时返回启动失败，不重复尝试配置或准入失败。`scheduler-stop` 在 worker 活动时请求协作取消，在退避期阻止下一次启动；其余队列保留。监督器收到正常取消时提交停止并等待子 worker 收尾。监督进程被强制结束时，Windows 的 Job Object 或 Linux 的父进程死亡信号会结束其子 worker；原任务之后仍按已有恢复规则处理。重启只能恢复具有原授权和安全回执的调度任务，不能增加 job 尝试次数上限、模型预算、期限或权限，也不能重放 `unknown` 动作。

后台启动成功回执保留普通 worker 的 `owner`、`pid`、generation 等字段，并增加 `supervised: true`、`supervisor_pid`、`max_restarts`、`restart_delay_ms`；`pid` 指本次确认就绪的 worker，`supervisor_pid` 指监督进程。后续 worker 重启会改变 worker PID 和 generation。启动回执只确认当时启动成功；后续应查询 `scheduler-status.supervisor` 或单独的 `scheduler-supervisor-status`，并结合持久记录与当次进程身份观测判断。不要将任何返回的 PID 当作结束进程的授权依据。

前台 `scheduler-supervise` 退出时返回：

```json
{"supervised":true,"state":"stopped","reason":"restart_limit","restarts":3,"last_exit_code":2,"supervisor_pid":1234,"run_id":"<本次监督运行 ID>"}
```

正常完成回执的 `reason` 为 `normal_exit`、`stop_requested`、`cancelled`、`restart_limit`、`startup_failed` 或 `ownership_changed`；`last_exit_code` 在尚无子进程退出回执时为 null。达到重启上限或启动失败时 CLI 退出码为 2。该完成回执描述本次监督循环；后台启动只返回启动确认，不会在原 stdio 连接推送监督完成通知。监督事件另有持久分页查询，模型任务的结果仍通过任务事件、任务和队列查询。

监督器和每次重启的 worker 使用监督器启动时的环境，因此环境引用的密钥轮换需要从新环境重启监督器。系统引用在每次任务/续跑开始时重新读取，不要求为轮换而重启；Linux 会话总线和密钥环仍须可用。`scheduler-start --supervise` 本身不会登记开机或登录自启；需另行使用下文的 `scheduler-autostart-enable --supervise`。监督进程本身崩溃后不会自我重启，也不能使已停止的 WSL 发行版、用户会话或主机自动启动。实际验证覆盖范围和 Windows 后台脱离限制见 [验证记录](validation.md)。

## 监督状态与事件

监督运行的阶段、配置和退出回执保存在状态库中。下面的命令通过只读连接查询，不创建数据库、启动监督器或修改原记录：

```powershell
codestudio-agent scheduler-supervisor-status --workspace D:\YourProject
codestudio-agent scheduler-supervisor-events --workspace D:\YourProject --after 0 --limit 50
```

`scheduler-status.supervisor` 与单独状态命令返回相同结构：

```json
{"record":null,"observation":{"state":"not_checked","observed_at_ms":1792800000000},"worker_matches":false}
```

没有监督记录时 `record` 为 null、观测为 `not_checked`。已有记录时 `record` 是该数据库最新一次监督运行的快照；普通 worker 后续接管不会删除它。主要字段如下：

| 字段 | 含义 |
|---|---|
| `id`、`owner`、`workspace` | 监督运行 ID、worker 所有者及绑定工作区 |
| `pid`、`process_identity` | 监督器 PID 与平台出生身份；身份字符串视为不透明值 |
| `phase` | 最后保存的 `starting`、`running`、`backoff`、`stopping` 或 `stopped` |
| `poll_ms`、`max_restarts`、`restart_delay_ms` | 本次监督循环的固定参数 |
| `restarts` | 已启动的替换 worker 次数，首次 worker 不计入；剩余可启动次数为 `max_restarts - restarts` |
| `worker_pid`、`generation`、`stop_epoch` | 最后关联的 worker 及停止代数；没有受管活动子进程时 `worker_pid` 为 0 |
| `last_exit_code` | 最近一次被监督器确认的子 worker 退出码；尚无回执时为 null，后续新 worker 运行时仍保留 |
| `next_restart_at_ms` | 退避时计划的重启时刻，其余阶段为 0；该时刻不保证重启一定发生 |
| `exit_reason` | 停止原因，未进入停止流程时为空字符串 |
| `started_at_ms`、`updated_at_ms`、`heartbeat_at_ms`、`finished_at_ms` | Unix 毫秒时间；未结束时 `finished_at_ms` 为 0 |

`worker_matches` 只比较同一数据库快照中监督记录与最后 worker 记录的 owner/generation 是否一致，不检查 worker 存活，也不表示它正在执行任务。持久 `phase` 和心跳是历史证据。进程被强制结束时，记录可能停留在 `running` 或 `backoff`；查询返回这些历史值，同时将当次 OS 观测放在独立的 `observation` 中：

| `observation.state` | 含义 |
|---|---|
| `matching_process` | 本次只读观测时存在与保存出生身份匹配的监督进程 |
| `not_running` | 本次观测到所绑定进程已退出，或 PID 当前不存在 |
| `identity_mismatch` | PID 存在，但出生身份与保存记录不同，不能将它认作原监督器 |
| `unavailable` | 权限、平台支持或身份读取等条件不足，无法可靠确认 |
| `not_checked` | 尚无监督记录，没有执行进程身份检查 |

`observed_at_ms` 标记这次查询观测的时间。数据库快照与 OS 观测不是跨系统原子事务；身份匹配只描述查询时刻，不保证进程健康、任务有进展或未来仍存活。监督器先保存 `stopped` 再退出，因此终态记录也可能暂时对应 `matching_process`。Windows 使用同一只读进程句柄核验创建时间与退出状态；Linux 使用 boot ID、`/proc` starttime 和 pidfd 前后退出检查，无法可靠绑定时返回 `unavailable`。查询不会根据 PID 控制进程，也不会因旧心跳生成退出回执或重放授权。

事件查询的 `--after` 默认 0，读取严格大于该游标的事件；`--limit` 默认 50、范围 1..100。返回 `items`、`next_after`、`has_more`，按 `seq` 递增；每条含 `seq`、`run_id`、`kind`、当时的完整记录 `payload` 和 `created_at_ms`。事件种类为 `supervisor.starting`、`supervisor.running`、`supervisor.backoff`、`supervisor.stopping`、`supervisor.stopped`、`supervisor.child_exited`。阶段与相应事件原子保存；周期心跳只更新快照，不生成事件。

客户端保存最后实际处理的 `seq`，重连后从该游标继续读取。监督事件拥有独立游标，不与任务 `events/list` / `events/subscribe` 共用，也不作为当前连接的推送通知。游标只适用于同一数据库，数据库替换或回滚后不能继续沿用。新监督运行取得准入锁后，若前一运行未保存终态，会将其记为 `stopped` / `superseded` 并追加事件；这只表示新运行接替旧记录，不伪造旧子进程的退出码或实际退出观察。内部异常可在持久记录中留下 `internal_error`；若进程被硬结束或数据库写入失败，最后记录仍可能保留旧阶段。

## Cron 周期任务

Cron 由 Windows/Linux 共用的调度循环执行，不写系统 crontab 或 systemd timer。worker 必须保持运行；它不会自动唤醒已关闭的 WSL 发行版或主机。

创建周期模板时显式提供提示词、模型配置和所需权限：

```powershell
codestudio-agent cron-create --workspace D:\YourProject `
  --name '工作日项目检查' --cron '0 9 * * 1-5' --timezone Asia/Shanghai `
  --prompt '阅读 README.md，报告需要关注的问题。' `
  --api responses --base-url https://your-provider.example/v1 --model your-model `
  --max-turns 8 --max-runs 20 --deadline-ms 3600000
codestudio-agent cron-list --workspace D:\YourProject
codestudio-agent cron-show <cron-id> --workspace D:\YourProject
codestudio-agent scheduler-start --workspace D:\YourProject
```

创建返回计划的 `id`、`expression`、`timezone`、`status`、`next_due_at_ms`、`last_due_at_ms`、`run_count`、`max_runs` 和 `last_job_id` 等字段。保存的是初始提示词、配置、原权限、冻结工具 schema 和零轮次的初始检查点，不创建任务、不请求模型、不自动启动 worker。如果配置了 MCP，创建阶段会实际启动明确授权的服务器完成发现并冻结 schema；插件启动本身遵循原沙箱和权限边界。

每次到期在同一事务中创建新任务、新检查点、job 和唯一触发记录，然后使用已有队列执行。`task_id` 与模型对话独立，上一轮答案、工具结果和审批不能自动成为下一轮的历史或授权；工作区文件仍是执行时的当前内容。每个任务从模板的完整 `max_turns` 开始，自己的重试和续跑继续累计消耗该预算。`--deadline-ms` 从该次任务生成时起算，包含其排队、审批与重试时间；`--max-attempts` 和 `--retry-delay-ms` 沿用前述范围。`--max-runs` 为 0..1000000，默认 0 表示无限次；达到非零上限后计划标为 `exhausted`，已生成的最后一个 job 仍可完成。`run_count` 统计已生成次数，不是成功次数。

worker 不在线或处理其他计划造成的多次遗漏合并成最近一个到期时刻，不逐条补齐积压。相同计划和触发时刻由数据库唯一键去重。同一计划上一轮仍排队、运行或等待审批时不生成重叠任务；当时到期的时刻按跳过处理，不消耗 `max_runs`，也不在上一轮结束后补回。任务结束与下次触发时间的推进同事务提交，人工接管后的运行和审批等待同样阻止重叠。等待审批不会产生新的提案副本，每一轮的新提案仍需独立决定。上一轮 job 被阻断或出现未知效果时，worker 自动暂停计划，防止下一轮绕过同一问题；恢复请求也不能跳过尚未核清的结果。

### 表达式与时区

表达式为五个数字字段，按所选时区解释：

| 字段 | 范围 |
|---|---|
| 分钟 | 0..59 |
| 小时 | 0..23 |
| 日期 | 1..31 |
| 月份 | 1..12 |
| 星期 | 0..7，0 和 7 都表示星期日 |

支持 `*`、逗号列表、包含端点的范围和正整数步长，例如 `*/15 9-17 * * 1-5`。`5/10` 从 5 起按步长选择该字段剩余范围；不支持秒/年份字段、月份或星期名称、`@daily`、`L`、`W`、`#` 等扩展。数字越界、倒序范围、零步长和永不发生的日期组合明确拒绝。

日期和星期均受限制时使用 **OR**：`0 9 1 * 1` 表示每月 1 日或每周一的 9 点。其中一个日字段含 `*` 或 `*/1` 时视为未限制，由另一日字段决定；`*/2` 等更大步长仍受限制，参与 OR。例如 `0 9 */2 * 1` 表示奇数日期或周一，而不是仅奇数日期中的周一。月份始终必须匹配。

`--timezone` 默认 `local`，创建时解析为当前系统的 IANA 时区名并固定保存；以后改变系统时区不会改写已有计划。也可显式指定 `UTC`、`Asia/Shanghai`、`America/New_York` 等名称。`UTC` 不依赖平台时区数据库；其他名称需要可用的 C++ 标准库时区数据，不可用时明确失败。夏令时跳过的本地分钟不执行；回拨时重复的分钟对应两个不同 UTC 时刻，均可触发，仍受不重叠规则约束。搜索最多覆盖约八年，UTC 时间范围为 1601..9999 年。

```powershell
codestudio-agent cron-preview --workspace D:\YourProject `
  --cron '30 1 * * *' --timezone America/New_York --count 5
```

预览不创建数据库或任务，返回每个时刻的 `at_ms`、`utc`、`local`，以及 `misfire: coalesce`、`overlap: skip`。可加 `--after <UTC epoch 毫秒>` 指定严格晚于该时刻的结果；默认当前时间。`--count` 默认 5，范围 1..100。

### 暂停、恢复和取消

```powershell
codestudio-agent cron-pause <cron-id> --workspace D:\YourProject
codestudio-agent cron-resume <cron-id> --workspace D:\YourProject
codestudio-agent cron-cancel <cron-id> --workspace D:\YourProject
```

`pause` 只阻止未来触发，不取消已经生成的工作。`resume` 从当前时间之后的下一个时刻继续，不补跑暂停期间的历史；被阻断或结果不明的上一轮必须先核清，次数已耗尽的计划不能恢复。`cancel` 永久取消计划，并取消其活动 job；运行中为协作取消请求，需要查询最终回执，不回滚已发生效果。已取消计划不能重新启用。计划状态为 `active`、`paused`、`cancelled`、`exhausted`，与每次 job 的状态分别记录。

## Windows 当前用户登录自启

```powershell
codestudio-agent scheduler-autostart-enable --workspace D:\YourProject --poll-ms 100
codestudio-agent scheduler-autostart-status --workspace D:\YourProject
codestudio-agent scheduler-autostart-disable --workspace D:\YourProject
# 显式登记监督模式；已有普通登记须先执行上面的 disable。
codestudio-agent scheduler-autostart-enable --workspace D:\YourProject --supervise `
  --max-restarts 3 --restart-delay-ms 1000
```

该功能通过 Windows Task Scheduler 登记**当前用户登录**触发，使用该用户的交互令牌和普通权限，无需保存账号密码。它不是系统服务，不提供用户登录之前的启动。登记固定当前 executable 的绝对路径、工作区、数据库、轮询间隔和运行模式，无需 shell 或启动脚本。默认直接执行 `scheduler-run`；`--supervise` 改为执行 `scheduler-supervise`，并固定 `--max-restarts`、`--restart-delay-ms`，默认值和范围与上文相同。`--poll-ms` 默认 100、范围 20..5000；普通模式拒绝显式监督限制。

任务身份由用户、工作区和数据库共同决定，切换模式不会创建另一份登记。已有同名登记的执行路径、模式、轮询间隔或监督限制不匹配时拒绝覆盖。修改这些配置前，应从原配置禁用登记，再以新配置启用；`status` 和 `disable` 自动识别已核验登记的模式及参数，无需重复传入监督选项。

`enable` 只创建或启用登记，返回 `started: false`，不会立即执行 Task Scheduler 的 Run，也不会绕过当前进程的父 Job 限制。当前会话要运行时另用 `scheduler-start`，监督模式需加 `--supervise`，或从适合的终端运行 `scheduler-run` / `scheduler-supervise`。`disable` 只移除匹配登记，阻止未来登录启动，不终止当前 worker 或监督器；需要停止时另用 `scheduler-stop`。`status` 返回 `available`、`registered`、`enabled`、`conflict` 和登记路径等信息，不证明 worker 已运行。

自启结果始终包含 `supervised`；为 true 时才包含 `max_restarts`、`restart_delay_ms`。在登记已核验且无冲突时，这些字段描述实际保存的配置；没有登记或存在冲突时，默认/请求值不证明已配置该模式。`disable` 的回执保留被移除登记的模式和限制。参数类型、数字范围、完整执行参数及平台元数据均参与归属校验，损坏或外来登记不能凭产品名称被删除。

Task Scheduler 的 `Hidden` 设置只隐藏计划任务登记项，**不保证登录启动的控制台进程没有窗口**。这与 `scheduler-start` 使用无窗口进程的启动路径不同。系统层的 `restart_count` 始终为 0：监督模式只允许监督器在自己的生命周期内有限重启子 worker，监督器自身退出或崩溃后不会被 Task Scheduler 自动拉起。后续登录触发可启动新的监督生命周期，重启计数从头开始；任务原有预算、期限、权限和未知效果约束不会重置。重复实例由 Task Scheduler 和执行侧独占锁约束。

自启登记不保存 API Key。使用环境引用的 worker 无法继承先前某个终端的临时变量；可在创建模型任务时选择 `--credential system:identifier`，由登录启动后的 worker 按任务绑定读取同一用户的系统凭据库。系统库必须在实际会话中可读取，登记成功不代表凭据可用或模型已认证。配置文件、模板与数据库仍受前述明文存储边界约束。

## Linux systemd 用户管理器自启

```sh
codestudio-agent scheduler-autostart-enable --workspace /home/user/project --poll-ms 100
codestudio-agent scheduler-autostart-status --workspace /home/user/project
codestudio-agent scheduler-autostart-disable --workspace /home/user/project
# 可在禁用旧登记后显式选择监督模式。
codestudio-agent scheduler-autostart-enable --workspace /home/user/project --supervise \
  --max-restarts 3 --restart-delay-ms 1000
# 需要在当前会话立即运行时，另行启动 worker。
codestudio-agent scheduler-start --workspace /home/user/project --supervise
```

Linux 后端要求运行 systemd，且 `/usr/bin/systemctl` 可执行。它在当前用户的 `${XDG_CONFIG_HOME}/systemd/user` 写入 `codestudio-agent-<摘要>.service`，并创建 `default.target.wants/<unit> -> ../<unit>`；未设置 `XDG_CONFIG_HOME` 时使用系统账户记录中的主目录下的 `.config`。配置目录必须是规范绝对路径，目录链不能包含符号链接，最终目录归当前 UID 所有且不可被其他用户写入。root 运行时登记的是 root 的用户单元，不会代替桌面用户登记，也不创建系统级服务。

返回 `backend: systemd-user`、`trigger: user_manager_start`、`user_uid` 和 `unit_path`。**触发条件是该用户的 systemd 管理器启动并到达 `default.target`**，不是每次打开终端或建立登录会话。通常首次登录启动用户管理器；如果它已在运行，新会话不会再次触发。已由用户或管理员开启 linger 的机器可能在系统启动时启动用户管理器，并在注销后保留它；本程序不会开启、关闭或修改 linger。没有 linger 时，最后会话结束后的清理可能停止用户管理器及其 worker，具体遵循发行版策略。

`enable` 只发布文件和启用链接，`disable` 只移除经核验的对应文件和链接，两者均不执行 `systemctl`、不 reload/start/stop 当前管理器，不启动或终止 worker。文件发生改变时返回 `requires_manager_reload: true`，表示当前管理器尚未通过本次命令重新加载；默认在**下一次用户管理器启动**时采用配置。`status` 的该字段不是当前管理器缓存状态的探测结果。需要现在运行或停止时使用 `scheduler-start` / `scheduler-stop`；disable 后仍可能存在已经运行或已被管理器加载的任务。

登记身份绑定 UID、规范化工作区和状态库路径，两种模式共用同一单元名称。单元固定 executable 路径、工作目录、运行模式、轮询间隔及监督限制；默认运行 `scheduler-run`，显式 `--supervise` 则运行 `scheduler-supervise`。更换 executable、模式、轮询间隔或监督限制前，先禁用原登记，再启用新配置。状态和删除自动读取并严格核验已保存配置，`supervised` 及条件限制字段的含义与 Windows 相同。

两种模式均使用 `Restart=no`，systemd 不会在退出后重新拉起 worker 或监督器，也不会通过重新启动监督器重置其有限重启计数。之后新的用户管理器启动触发可开启新的监督生命周期；任务原有授权、期限和预算仍保留。标准输入/输出指向 null，标准错误进入用户 journal。命令不经过 shell，关闭 systemd 参数环境展开，并转义路径中的 `%` 等语法字符。`WorkingDirectory` 使用完整路径值，与 `ExecStart` 的参数引号规则分开；含控制字符以及以空格或反斜杠结尾的工作区路径明确拒绝，避免单元解析改变路径。登记文件不包含 API Key 或环境快照；自启进程继承用户管理器的实际环境，不能依赖启用命令所在 shell 的临时变量。

`status` 只读取本地登记文件。`available: true` 不保证用户管理器、用户总线或未来登录触发当前可用，`enabled: true` 也不证明服务已加载、正在运行或模型已认证。管理命令核验完整单元内容、当前 UID、文件类型以及启用链接目标；外来或被修改的单元、符号链接单元、孤立启用链接和外来链接均拒绝覆盖或删除。创建采用不替换已有文件的原子发布，删除前复查文件身份。该边界面向可信的当前用户配置，不审计 systemd 全局/用户 drop-in，也不抵御恶意同 UID 并发修改或管理员控制。

WSL 需要先启动发行版并启用 systemd，之后还取决于该用户管理器何时启动。此登记不会自动启动 WSL 发行版，也不注册 Windows 登录任务；Windows 登录、启动 WSL 发行版和 Linux 用户管理器启动是三个不同事件。可用性探测和临时目录下的登记测试不能证明实际重启/登录触发已经发生。

## stdio 接入

协议版本仍为 2。`initialize` 返回 `background_scheduler: true`、`automatic_resume: true`、`cron: true`；Windows/Linux 的 `worker_supervision: true` 表示支持显式有界监督，`supervisor_diagnostics: true` 表示提供上述持久记录和进程身份诊断，`login_autostart: true` 和 `supervised_autostart: true` 分别表示已编译自启后端及其监督模式接入，运行时可用性仍查 `scheduler/autostart/status`。其他平台的这四个能力为 false。Windows 返回 `trigger: current_user_logon`，Linux 返回 `trigger: user_manager_start`。这些能力表示客户端可显式创建持久登记与控制 worker，不表示启动服务就自动执行历史任务或修改登录设置。

| 方法 | 参数 | 返回值 |
|---|---|---|
| `schedules/create` | `task_id`，可选 `options`、`delay_ms`、`deadline_ms`、`max_attempts`、`retry_delay_ms` | 完整 job，`id` 为 job ID |
| `schedules/list` | 可选 `before`、`limit` | `items`、`next_before`、`has_more` |
| `schedules/get` | `job_id` | 完整 job |
| `schedules/cancel` | `job_id` | 更新后的 job |
| `scheduler/status` | 空对象 | 最后登记的 worker 状态及 `supervisor` 诊断对象 |
| `scheduler/supervisor/status` | 空对象 | `record`、`observation`、`worker_matches` |
| `scheduler/supervisor/events` | 可选 `after`、`limit` | 监督事件 `items`、`next_after`、`has_more` |
| `scheduler/start` | 可选 `poll_ms`、`supervise`；监督时可加 `max_restarts`、`restart_delay_ms` | 确认启动后的状态及 `started: true`；监督时附带监督配置 |
| `scheduler/stop` | 空对象 | 状态及 `stop_requested: true` |

这些方法是同步控制，不占用前台 Operation 槽，也不会返回 operation ID。活动前台操作期间仍可查询、取消或请求停止；创建登记仍需通过任务执行锁及全部校验，不能绕过 busy。所有写操作必须有请求 ID，无 ID 通知不改变队列或启动 worker。

`options` 复用 `tasks/resume` 的字段与约束，拒绝 `prompt`、`workspace`、`state`、`api_key` 和显式 `stream: true`；省略流式选项沿用原模式。可选 `credential` 必须与保存引用一致，省略时从检查点恢复；凭据管理本身只通过 CLI 提供。时间和次数选项范围与 CLI 相同。示例：

```json
{"jsonrpc":"2.0","id":20,"method":"schedules/create","params":{"task_id":"source-task-id","options":{"allow_build_proposals":true},"deadline_ms":86400000,"max_attempts":8}}
{"jsonrpc":"2.0","id":21,"method":"scheduler/start","params":{"poll_ms":100}}
{"jsonrpc":"2.0","id":22,"method":"schedules/list","params":{"limit":50}}
```

后台执行不产生本次 stdio 连接的 `operation/text` 或 `operation/finished`。客户端通过原有持久事件订阅、任务查询和队列查询观察进度；断连后按事件 seq 重放。完整连接生命周期见 [本地任务服务](task-service.md)。

Cron 的 `crons/create` 与任务启动一样采用异步 Operation，因为创建可能启动已授权 MCP 做工具发现。它先返回 `operation_id` / `accepted`，完成通知的 result 才是计划记录；该操作不请求模型。`crons/list|get|preview|pause|resume|cancel` 和 `scheduler/autostart/status|enable|disable` 为同步控制，参数和生命周期见 [周期与自启 RPC](task-service.md#周期任务与登录自启)。所有写请求都必须有 ID；创建计划与每次后续后台执行分别观察。

```powershell
python tests/scheduler_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/cron_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/autostart_integration.py build/windows/x64/debug/codestudio-agent.exe
```

当前平台与实际回归结果以 [验证记录](validation.md) 为准。
