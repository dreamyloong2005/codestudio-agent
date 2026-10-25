# 构建提案与审批

Agent 可以通过 `propose_build` 提出完整构建清单。Harness 校验并保存提案，用户查看原清单和规范化后的执行计划，再按摘要批准或拒绝。只有批准才会创建独立构建任务，并通过现有 Windows LPAC 后端执行。

该能力需要显式 `--allow-build-proposals`。提案动作成功时 `effect` 为 `none`，不导入源码或工具链、不启动构建、不导出产物。原模型任务暂停为 `waiting_approval`，CLI 返回退出码 3，并提供 `approval.action_id` 和 `approval.digest`。提案对应的工具调用暂不回复模型，同一批次中后续工具也等待续跑。

已有的 `build --build-config <清单>` 和模型 `build_workspace` 工具接受用户预先提供的固定 v2 清单。`--allow-build-proposals` 与 `--build-config` 可以同时使用：前者允许模型提交待审批方案，后者允许执行已经授权的固定方案。通用显式步骤格式和隔离边界见 [工作区隔离构建](workspace-builds.md)。

## CLI 流程

先配置模型凭据，再显式开放提案工具。例如：

```powershell
codestudio-agent run --workspace D:\YourProject `
  --base-url https://your-provider.example/v1 --model your-model `
  --prompt '阅读工程，使用我提供的工具链路径提出 v2 隔离构建方案，说明所需输入和产物。' `
  --allow-build-proposals
```

工具链路径必须由用户提供或已有工作区信息确定；宿主不会自动发现、安装或下载工具。模型仍需显式调用 `propose_build` 才会生成可审批回执，普通回答中的 JSON 不是提案记录。

使用任务输出中的任务 ID 查看动作：

```powershell
codestudio-agent show <task-id> --workspace D:\YourProject
codestudio-agent build-review <task-id> --action <action-id> --workspace D:\YourProject
```

`action-id` 是 `show` 中 `actions` 的账本动作 ID，也可从持久化动作事件获取；它不是模型的 tool call ID。选择成功的 `propose_build` 动作。`build-review` 只读，不创建或迁移数据库，返回：

```json
{
  "task_id": "source-task-id",
  "action_id": "source-action-id",
  "proposal": {
    "version": 2,
    "status": "pending_approval",
    "workspace": "D:/YourProject",
    "reason": "构建并验证项目产物",
    "manifest": {"version": 2},
    "recipe": {"version": 2},
    "protected_paths": [],
    "backend": "windows-lpac-build-v2",
    "network": "none",
    "export": "new-atomic-bundle",
    "digest": "<64 位小写 SHA256>"
  },
  "decision": null
}
```

上例省略了实际清单、完整计划和保护路径；真实响应提供其完整内容。提案回执、清单和执行计划均使用 version 2，不包含插件标记。查看 `manifest` 与 `recipe` 中的输入、工具链副本、可执行文件、参数、环境、资源限额和产物目录后，选择以下一个决定：

```powershell
codestudio-agent build-approve <task-id> --action <action-id> `
  --digest <本次查看的摘要> --workspace D:\YourProject

# 或拒绝该方案
codestudio-agent build-reject <task-id> --action <action-id> `
  --digest <本次查看的摘要> --workspace D:\YourProject
```

如果原任务使用自定义数据库，以上所有命令都应提供相同的 `--state <数据库>`。审批入口要求显式工作区、动作 ID 和精确的 64 位小写十六进制摘要；不接受修改已审阅的清单或构建命令。默认批准或拒绝操作本身只完成该决定及必要的独立构建，不调用原任务模型；已单独登记的后台 worker 可在结果就绪后续跑。

批准结果包含 `proposal`（最新 review 对象）、`execution`（独立执行任务详情）和 `state_database`。拒绝时 `execution` 为 `null`。批准执行成功返回退出码 0，取消为 130，其他非完成执行状态为 1；参数、配置或授权错误为 2。拒绝成功返回 0。

## 模型工具参数与摘要

`propose_build` 严格接受 `manifest` 和 `reason` 两个字段。`manifest` 只接受 version 2，必须显式声明通用步骤。以下方案中的 `my-tool.exe` 是占位工具，需替换为项目实际程序、参数和输入，将示例目录换成真实工具路径，并确保所需依赖也在清单内：

```json
{
  "manifest": {
    "version": 2,
    "inputs": ["input.txt"],
    "mounts": [
      {"name": "tools", "source": "D:/YourTools", "inputs": ["my-tool.exe"]}
    ],
    "steps": [
      {
        "executable": "${tools}/my-tool.exe",
        "arguments": ["input.txt", "--output", "result.bin"]
      }
    ],
    "outputs": ["result.bin"],
    "artifact_directory": "artifacts",
    "limits": {"timeout_ms": 120000, "processes": 16, "memory_mb": 2048}
  },
  "reason": "使用已提供的工具生成 result.bin，并将产物导出到新包。"
}
```

方案应根据项目实际工具显式声明 `steps` 和所需环境，宿主不会推断工具命令。完整格式见 [v2 清单](workspace-builds.md#固定清单)。提案阶段不读取 mount 文件，也不证明工具链完整或构建会成功；实际导入、文件检查和执行发生在批准以后。

宿主对去掉 `digest` 字段后的提案对象，以当前 JSON 库 `dump()` 生成的精确 UTF-8 字节计算 SHA256；对象键排序、无额外空白。绑定内容包含原始清单、规范化计划、格式版本、工作区、后端、网络与导出规则，以及宿主生成的 `protected_paths`。保护路径来自宿主政策，模型不能通过工具参数设置或删减。

查看与审批均会重新规范化清单，并与保存的整个回执精确比较。工作区不匹配、回执被改动、执行计划发生变化、摘要错误或内容需要已知凭据脱敏时均拒绝。客户端应提交用户实际查看并确认的摘要，不应在失败后静默替换成新摘要重试。

批准执行本身不调用模型，但新执行回执需要按源任务保存的凭据引用脱敏，所以该引用必须可读取；缺失、锁定或不可用时不会启动新构建。单独拒绝仍可在密钥不可用时提交。批准与显式继续合并时，在一次操作中固定使用同一个已解析的值。密钥值不保存在提案、绑定或审批参数中，系统存储与轮换边界见 [模型凭据与轮换](credentials.md)。

提案记录只使用当前 schema 7 状态库；其他状态库版本会在读取入口拒绝，不转换回执或摘要。请使用新状态库或新工作区创建任务，再生成和审阅新提案。

摘要标识的是配置，**不包含源码和编译器的历史字节快照**。批准执行导入的是当时路径上的文件；需要固定源码版本或工具链版本时，应先由用户准备对应目录，再提出方案。

## 决定、执行与恢复

每个提案只保存一个决定。批准会在同一 SQLite 事务中写入决定、创建 `provider: "approved-build"` 的独立任务、prepared 动作和审计事件，提交后才派发执行。执行直接使用冻结计划和通用 `WorkspaceExecution`，保留原任务保护路径并补充当前数据库及 sidecar 路径；LPAC、资源限制、取消和新产物包的原子导出语义与固定构建一致。

| 情况 | 行为 |
|---|---|
| 首次批准 | 保存决定并执行一次独立构建任务 |
| 重复相同批准 | 查询原执行任务及结果，不再次派发 |
| 首次拒绝或重复拒绝 | 保存或返回拒绝决定，不创建执行任务 |
| 将已拒绝提案改为批准，或反向操作 | 拒绝冲突决定 |
| 执行失败、取消或结果不明后再次批准 | 返回原任务，不重试；再次尝试需新提案 |
| 宿主崩溃 | 先 `recover`，保留审批决定，不释放为可重试状态 |

原提案回执保持 `pending_approval`，当前决定以 review 的独立 `decision` 字段为准。决定包含 `approved` 或 `rejected`、摘要及关联执行任务/动作 ID；拒绝时执行标识为空值。

崩溃恢复区分未派发与结果不明动作，但即便批准任务停在 prepared 阶段，也不会因重复批准或恢复而自动执行。批准任务没有模型检查点，不能使用 `resume`。可能已经发布但尚未记录完成的产物需要人工核对。

## 回注结果并继续原任务

决定完成后，可显式恢复原任务。需重复其全部授权；下面仅演示原任务只开放提案的情况：

```powershell
codestudio-agent resume <task-id> --workspace D:\YourProject --allow-build-proposals
```

也可在批准或拒绝时请求继续，附带同样的原任务授权和必要配置：

```powershell
codestudio-agent build-approve <task-id> --action <action-id> `
  --digest <本次查看的摘要> --workspace D:\YourProject `
  --continue-task --allow-build-proposals

# 拒绝后向原模型说明决定并继续
codestudio-agent build-reject <task-id> --action <action-id> `
  --digest <本次查看的摘要> --workspace D:\YourProject `
  --continue-task --allow-build-proposals
```

原任务如有写入、进程、MCP、回环或沙箱授权，必须原样再次提供；模型、端点和凭据引用默认沿用保存配置，本次操作按 `env:VARIABLE` 或 `system:identifier` 重新解析密钥；显式 `--credential` 必须匹配原引用。续跑仍核对同一任务、工具 schema、配置及权限绑定，不能通过审批入口增加权限。合并操作必须沿用原任务保存的累计轮次预算，不能通过 `--continue-task` 改变预算；需要提高时，另行使用显式 `resume --max-turns`。

仅当原任务仍处于 `waiting_approval`，且检查点精确等待本次提案时，合并操作才开始续跑。决定尚未完成、批准任务仍在运行或执行效果不明时不能继续。请求被取消，或原批准执行已取消时，不通过合并操作启动续跑。重复决定不会推进已完成、已经续跑或正在等待另一个提案的任务；若之前只完成决定、源任务仍等待同一提案，则显式请求继续仍可通过精确匹配检查。

回注前，宿主重新对保存的完整提案 `output` 去掉 `digest` 后按规范 JSON 字节计算 SHA256，并核对提案摘要及决定摘要。即便摘要字段本身没变，提案正文被修改也会拒绝。新增的执行反馈还必须通过本次选中密钥及既有 `CODESTUDIO_API_KEY` 的精确脱敏检查：如果脱敏会改变待回注内容，续跑在启动 MCP 或请求模型之前拒绝，不把改写后的反馈交给模型。

回注使用原模型 tool call ID，将 `proposal`、`decision` 和 `execution` 放入一个延迟的工具结果。拒绝时 `execution` 为 `null`；批准时包含独立构建动作的状态与回执。该消息只表示交付决定，构建的实际效果仍以独立执行回执为准。宿主不会改写原始提案回执或再次调用构建工具。工具结果、检查点游标推进和 `build.proposal.consumed` 事件在同一事务中提交，避免崩溃后重复回注。

合并操作保留原 `proposal`、`execution`、`state_database` 字段，并增加 `continuation`：

| `continuation.status` | 其余字段 | 含义 |
|---|---|---|
| `started` | `result`、`exit_code` | 已运行原任务续跑；查看其实际结果和退出码 |
| `skipped` | `reason` | 当前状态或取消不允许启动本次续跑 |
| `failed` | `error` | 续跑校验或启动失败；已提交的审批决定不回滚 |

`started` 不代表原任务已经完成，它可能再次暂停等待新提案、耗尽预算或失败。决定和续跑是两个阶段：批准可能已经提交并完成构建，随后续跑才因授权、配置、摘要或凭据冲突失败；这时批准决定、独立执行任务和已有产物不会回滚，合并返回仍保留其回执，不能把顶层失败理解为“没有执行”。此时顶层退出码沿用续跑退出码；`failed` 返回 2，取消返回 130；`skipped` 保留决定阶段的退出码，取消除外。继续失败时应先检查决定和原任务状态，再使用显式 `resume`；不能重新批准来重放构建。完整边界见 [续跑规则](resume.md)。

也可在等待决定时，用 `schedule-resume <task-id>` 明确重复原授权并保存续跑登记，再启动独立 worker。随后照常审阅并批准或拒绝；worker 只在当前决定及执行回执就绪后恢复同一任务。新的提案仍会暂停并要求新的明确审批，持久续跑授权不会批准构建。登记保留原预算并受期限和尝试次数约束，等待审批也计入期限。成功手动续跑会取消原排队登记；worker 每次续跑按保存引用读取凭据；系统库轮换在下一次操作生效，环境引用仍需从新环境重启 worker。详见 [后台续跑](scheduler.md)。

## stdio 客户端

`initialize` 的 `capabilities.build_proposals: true` 表示支持提案，`approval_continuation: true` 表示支持审批后显式续跑；`automatic_resume: true` 和 `background_scheduler: true` 表示支持另行显式登记的后台续跑。`tasks/start` 的 `mode: "run"`、`tasks/resume` 及 `schedules/create` 可使用 `options.allow_build_proposals: true`。服务启动时固定工作区和数据库；完整生命周期见 [本地任务服务协议](task-service.md)。

```json
{"jsonrpc":"2.0","id":10,"method":"builds/review","params":{"task_id":"source-task-id","action_id":"source-action-id"}}
{"jsonrpc":"2.0","id":11,"method":"builds/approve","params":{"task_id":"source-task-id","action_id":"source-action-id","digest":"<本次查看的64位小写SHA256>"}}
```

若选择拒绝，使用以下请求代替批准：

```json
{"jsonrpc":"2.0","id":11,"method":"builds/reject","params":{"task_id":"source-task-id","action_id":"source-action-id","digest":"<本次查看的64位小写SHA256>"}}
```

`builds/review` 同步只读，活动任务期间也可查询。`builds/approve` 和 `builds/reject` 使用现有单活动操作 worker，先返回 `{"operation_id":"...","status":"accepted"}`，再通过 `operation/finished` 返回最终结果。接受请求不代表批准或执行已经完成；可用 `operations/cancel` 请求取消批准执行，并等待最终账本状态。

`builds/approve` 和 `builds/reject` 还接受可选的 `continue_options` 对象，其字段和校验复用 `tasks/resume.options`。只要提供该对象（包括 `{}`）就请求继续；需要在其中重复原授权，例如：

```json
{"jsonrpc":"2.0","id":12,"method":"builds/approve","params":{"task_id":"source-task-id","action_id":"source-action-id","digest":"<本次查看的64位小写SHA256>","continue_options":{"allow_build_proposals":true}}}
```

`continue_options` 不接受 `workspace`、`state`、`api_key` 或 `prompt`，`stream: true` 与普通续跑一样拒绝；省略 stream 沿用原模式。`builds/review` 不接受该选项。审批及续跑共用同一个活动 operation，流式文本也带同一 operation ID，最终通过 `operation/finished` 返回上述合并结构。

三个方法均不能覆盖服务的固定工作区、数据库或已审阅清单。没有请求 ID 的通知不执行审批。持有 stdio 管道的客户端被视为可信授权方，应负责向用户展示方案并取得决定；审批方法不作为模型工具或 MCP 工具暴露。

## 限额与验证

提案参数序列化后最多 64 KiB，`reason` 为 1–4096 个 UTF-8 字节且不含 NUL，完整回执最多 128 KiB。v2 清单的路径、环境、步骤、挂载和资源限额继续适用。宿主会检查已配置凭据是否出现在字符串或对象键中，但不能识别所有秘密；提案、理由和回执会进入未加密的任务数据库。

提案校验与批准执行支持 Windows LPAC 和 Linux namespace/cgroup 工作区隔离后端，macOS 明确拒绝。平台能力和限制见 [隔离边界](workspace-builds.md#隔离边界) 和 [Linux 指南](linux.md)。Linux 专属审批、构建与导出验证在 tests/linux_build_integration.py 中。可运行提案集成测试：

```powershell
python tests/build_proposals_integration.py build/windows/x64/debug/codestudio-agent.exe
```

实际构建与回归验证记录见 [验证结果](validation.md)。
