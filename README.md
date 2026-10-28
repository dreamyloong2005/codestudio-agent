# CodeStudio Agent

以 **C++26 + XMake** 构建的本地 Agent/Harness。当前为 **0.1.0 CLI 基础版本**：模型通过受控工具操作指定工作区，任务、工具意图和结果保存到 SQLite。

**CodeStudio Agent** 负责无界面的执行内核、CLI、本地任务服务、工具权限、隔离和调度。桌面 UI 由独立项目 **CodeStudio Next** 负责，通过任务服务接口接入；界面实现及其技术选型归该项目管理。

长期架构见 [研究报告](Agent与Harness研究及新产品架构建议.md)。未来随着 mcpp 生态成熟再评估迁移；当前工程使用 XMake。

用户级安装、升级、卸载、Windows/Linux 标准发布包与统一验证入口见 [构建与交付](docs/delivery.md)。`python scripts/verify.py --mode all` 顺序验证 Debug/Release 并生成独立 JSON 回执；发布工具只打包已构建程序。安装不改 PATH、自启、用户工作区或凭据。CI 定义已提供，托管执行需要另行配置符合要求的 runner。

## 当前能力

- **隔离资源管理**：新沙箱记录所有权和进程出生身份，支持只读列表、回收预览与单 ID 显式回收；不认领未登记的历史目录，不重放未知动作。见[资源登记与回收](docs/sandbox-resources.md)。
- 可配置的 **Chat Completions、OpenAI Responses、Anthropic Messages** 接口：默认 SSE 流式响应，支持非流式回退、函数工具调用、有限轮次、超时、响应限长和 Ctrl+C 取消。
- 无需密钥的离线演示：脚本化 provider 写入文件、读回并核对内容。
- `read_file`、`write_file`、`run_process` 三种工具；写入和进程执行分别授权。
- SQLite 任务、动作与事件记录；先持久化执行意图，再调用工具。
- `history`、`show` 查看记录与检查点摘要；`recover` 标记中断任务和结果不明动作；`resume` 在核对配置、权限和回执后继续同一任务。
- **后台续跑队列**：显式登记已有任务的原授权，由独立 worker 按延迟、期限、退避及原预算续跑；支持审批等待、取消和安全崩溃恢复。Windows/Linux/macOS 后台启动独立于 stdio 客户端，worker 自身不自动重启。见 [后台续跑](docs/scheduler.md)。
- **Cron 与平台自启**：Windows/Linux/macOS 共用五字段 cron、固定时区及触发预览；每次生成独立任务与预算，支持暂停/取消、遗漏合并和防重叠。Windows 登记当前用户登录触发；Linux 登记 systemd 用户管理器启动触发；macOS 使用当前用户 LaunchAgent。登记不立即启动、不保存账号密码。见 [周期任务与自启](docs/scheduler.md#cron-周期任务)。
- Windows Job Object 管理进程树；POSIX 后端使用进程组。包含超时、取消、输出限长和凭据环境过滤。
- Windows 可选 `--sandbox appcontainer`：普通进程与 stdio MCP 使用 LPAC、无网络 capabilities、单 executable 暂存和私有 scratch。默认仍为 `none`；隔离边界与验证说明见 [Windows 沙箱](docs/sandbox.md)。
- Linux 可选 `--sandbox linux-namespace`：Bubblewrap 私有根和命名空间、seccomp、cgroup v2，支持普通工具及 stdio MCP 的单进程隔离。需要明确的系统委派，缺失时拒绝执行。见 [Linux 与 WSL](docs/linux.md)。
- **Windows/Linux 工作区隔离构建**：通用宿主执行模块负责源码/工具链副本、受限进程树、CPU/内存/进程数限制、取消与原子产物导出；仅接受 v2 清单，显式声明步骤和所需环境。支持直接 `build`、模型 `build_workspace` 和任务服务。见 [隔离构建](docs/workspace-builds.md)。
- **构建提案与审批**：显式开放 `propose_build` 后，模型提交 v2 清单并暂停为 `waiting_approval`。通过 CLI 或可信 stdio 客户端按摘要批准或拒绝，再显式续跑原任务；也可在审批时提供原授权，一次完成决定与续跑。批准创建独立隔离构建任务，重复请求不重放构建或推进后续会话。见 [构建提案](docs/build-proposals.md)。
- **MCP 工具扩展**：stdio 与 Streamable HTTP、显式服务器/工具授权、独立凭据引用、JSON/SSE、GET 断线恢复及统一回执；未知效果不重放。
- **本地任务服务**：`serve` 通过 stdio JSON-RPC 提供任务查询、启动/续跑/取消、事件游标订阅和实时模型文本，与 CLI 共用执行入口。提供机器可读协议、结构化错误、会话标识和有限操作回执查询，供 CodeStudio Next 等可信客户端接入。
- **本地模型诊断**：`models` 查询已有服务的模型列表；`model-check` 检查流式响应和两轮合成工具往返，无需云端密钥。

`completed` 表示模型循环正常结束，**不等于独立验收通过**。工具失败后，即便模型声称完成，也会标为 `needs_attention`；进程或写入结果不明时停止自动推进。

## 构建

本次实测环境：Windows x64、XMake `3.0.8+dev.1e431c685`、Clang/clang-cl `22.1.8`、Visual Studio 2022 Build Tools（MSVC 标准库 `14.44.35207`）、Windows SDK `10.0.26100.0`。C++ 语言模式实测为 `202400`。未升级本机工具。

```powershell
xmake f -p windows -a x64 --toolchain=clang-cl -m debug -y
xmake build -a
xmake run codestudio-agent doctor
```

构建时检查 C++26 模式，工具链不支持时明确失败。暂未依赖 reflection、contracts 或 `std::execution`，这些能力将分别验证后启用。

`set_xmakever("3.0.5")` 是最低版本要求；实际开发/CI 应固定通过验证的 XMake 精确版本。`xmake-requires.lock` 记录 Windows x64 与 macOS arm64 依赖；默认 SQLite、JSON 库采用固定版本。Linux 已在 Arch WSL2、GCC 16、XMake 3.1.0 下构建运行，`--system_deps=y` 可使用发行版 pkg-config 开发包；HTTP 使用 libcurl，摘要使用 OpenSSL，系统凭据使用 libsecret；Linux 始终需要系统 libsecret-1 开发包和运行库。WSL 独立镜像、构建和验证命令见 [Linux 指南](docs/linux.md)。macOS 已加入 Apple Silicon 基础运行适配和统一验证入口，原生 Debug/Release 验收通过；工具链与能力边界见 [macOS 指南](docs/macos.md)。

macOS arm64 使用 LLVM 22.1.8 及配套、启用时区数据库的 libc++；最低部署目标为 macOS 15.0，本轮仅在 26.5.2 实测。系统 Apple libc++ 和官方 macOS LLVM 包附带的 libc++ 均未启用所需时区功能；先按 [macOS 指南](docs/macos.md) 构建专用运行库，再运行：

```sh
export CODESTUDIO_LLVM_ROOT=/absolute/path/to/llvm
export CODESTUDIO_LIBCXX_ROOT="$PWD/build/toolchains/libcxx-22.1.8-tzdb"
export CODESTUDIO_CONTRACT_PYTHON=/absolute/path/to/python3
bash scripts/verify-macos.sh all
```

验证会实际编译和运行 C++26、`std::format`、时区与 DST 探针，并生成 `build/verification/macosx/` 下的独立回执。macOS 支持环境与 Keychain 凭据、OAuth 原生存储、后台启动与有界监督及 LaunchAgent 登记。真实登录触发须按 [macOS 验收步骤](docs/macos.md) 单独验证；隔离执行仍不可用，使用 `--sandbox none`。构建提案可创建、审阅、拒绝并续跑；批准请求因隔离后端缺失而在提交决定前拒绝。安装与签名公证属于后续阶段。

## 无需密钥体验

```powershell
xmake run codestudio-agent demo --workspace .demo-workspace
xmake run codestudio-agent history --workspace .demo-workspace
xmake run codestudio-agent show <task-id> --workspace .demo-workspace
```

演示会在指定目录创建/写入 `CODESTUDIO_DEMO.txt`，然后读回验证，输出 JSON 结果。它使用明确标记的 `offline-demo` provider，不调用云端模型。请选择专用演示目录。

通过 `xmake run` 启动时工作目录固定为项目根目录；直接运行可执行文件时使用当前终端目录。上述相对路径会在项目根目录下创建 `.demo-workspace`。

任务状态默认保存在 `<workspace>/.codestudio/state.sqlite3`。可使用 `--state <path>` 指定数据库；同一数据库和同一工作区同时只允许一个 CLI runner。

## 接入模型

使用凭据引用选择密钥，默认 `env:CODESTUDIO_API_KEY`；也可指定 `--credential env:YOUR_VARIABLE` 或 `--credential system:work-model`。密钥值不放进命令行参数、任务配置或项目文件。环境来源示例：

```powershell
# 在当前会话中，通过你自己的凭据管理方式设置 CODESTUDIO_API_KEY。
$env:CODESTUDIO_BASE_URL = 'https://your-provider.example/v1'
$env:CODESTUDIO_MODEL = 'your-chat-completions-model'

xmake run codestudio-agent run --workspace D:\YourProject `
    --prompt '阅读 README.md，说明这个项目的构建步骤。'
```

系统凭据支持 Windows 当前用户 Credential Manager、Linux Secret Service 和 macOS Keychain。使用 `credential-set --name work-model --stdin` 从重定向 stdin 设置/更新，再通过 `credential-status --credential system:work-model` 查询可用状态，`credential-delete --name work-model` 删除。设置不会从普通终端回显读取，安全输入示例与平台条件见 [模型凭据与轮换](docs/credentials.md)。

系统库每次新任务或续跑读取一次，轮换后后续任务使用新值，正在执行的操作保持原值；缺失、锁定或不可用均失败，不回退来源或因认证错误隐式重放。引用保存在精确执行绑定中；缺少引用的旧模型检查点拒绝续跑，不做转换。数据库仍为 schema 7，协议和检查点仍为 version 2。

也可使用 `--base-url`、`--model` 参数覆盖环境配置。通过 `--api` 或环境变量 `CODESTUDIO_API` 选择协议，默认 `chat-completions`：

| `--api` | 端点后缀 | 认证 | 工具协议 |
|---|---|---|---|
| `chat-completions` | `/chat/completions` | Bearer | `tool_calls` / `tool` |
| `responses` | `/responses` | Bearer | `function_call` / `function_call_output` |
| `messages` | `/messages` | `x-api-key`，`anthropic-version: 2023-06-01` | `tool_use` / `tool_result` |

`--base-url` 可填写带 `/v1` 的基础地址，或与所选协议一致的完整端点。选择支持对应接口和函数工具的模型，模型名必须显式配置。

```powershell
# 通过自己的凭据管理方式设置 CODESTUDIO_API_KEY 后运行。
xmake run codestudio-agent run --workspace D:\YourProject `
    --api responses --base-url https://api.openai.com/v1 `
    --model your-responses-model --prompt '阅读 README.md 并说明构建步骤。'

xmake run codestudio-agent run --workspace D:\YourProject `
    --api messages --base-url https://api.anthropic.com/v1 `
    --model your-claude-model --max-output-tokens 4096 `
    --prompt '阅读 README.md 并说明构建步骤。'
```

默认发送 `stream=true`。文本增量实时写入 **stderr**，最终任务 JSON 写入 **stdout**；增加 `--no-stream` 可使用普通 JSON 响应。`--max-output-tokens` 设置 Responses 的 `max_output_tokens` 或 Messages 的必填 `max_tokens`（默认 4096）。

流式文本是临时展示：连接中断、超时、服务端报错或缺少成功结束事件时，该轮不能执行工具。工具参数必须完整解析、校验并通过权限检查，才会持久化意图和执行。密钥跨多个文本片段时仍会脱敏，ASCII 控制字符会转义（保留换行和制表符）。

Responses 使用 `store=false` 和完整输入历史，保留推理 continuation 所需的原生输出；Messages 保留原生内容块与签名用于后续轮次。CLI 只展示回答文本，不展示推理内容。本版未开放多模态输入、内置托管工具、供应商专用推理配置或服务端续传；兼容范围仍需依据实际模型/网关验证。

默认只允许读取工作区文件。需要修改文件时加 `--allow-write`；需要运行进程时，用一个或多个 `--allow-process <absolute-executable>` 明确授权：

```powershell
xmake run codestudio-agent run --workspace D:\YourProject `
    --prompt '运行指定的构建工具并报告结果。' `
    --allow-process 'D:\Dev\Tools\xmake\xmake.exe' `
    --max-turns 8 --process-timeout-ms 60000
```

进程不经过隐式 shell。默认 `--sandbox none` 将工作目录设为工作区，**授权一个可执行文件，也授权了它及其子进程以当前用户权限运行**；参数仍由模型产生，授权 shell、Python、Git 或构建工具可能获得广泛能力。

Windows 可添加 `--sandbox appcontainer`，将普通进程和 stdio MCP 服务器放入 LPAC。此时只暂存一个 executable，工作目录为私有 scratch，单进程 Job 阻止直接后代运行，不授权原工作区，不复制旁边的 DLL/脚本，也不回写 scratch。需要完整构建时，请改用 `build --workspace <目录> --build-config <清单>`，或者为模型任务提供 `--build-config`，详见 [隔离构建清单](docs/workspace-builds.md)。普通 `--sandbox appcontainer` 继续面向单进程自包含工具。宿主文件工具与模型 HTTP 连接继续遵循各自权限。详见 [沙箱使用与限制](docs/sandbox.md)。

远端端点必须使用 HTTPS，不跟随重定向。访问本地无 TLS 模型服务时，显式添加 `--allow-http-loopback`，且地址必须是 `127.0.0.1` 或 `[::1]`。模型接口按用户配置访问；文件内容可能发送给该服务。

## 桌面客户端接入

`codestudio-agent serve --workspace <已有目录> [--state <数据库>]` 由客户端作为子进程启动，stdin/stdout 必须为管道。工作区和数据库在服务启动时固定，工具授权通过每次任务请求显式提供；服务不监听网络端口。

接口支持任务分页/详情、已提交事件订阅与重放、异步启动/续跑、取消和关闭，以及持久续跑队列、cron、独立 worker 和登录自启控制。实时模型文本是临时展示，最终状态仍以任务回执为准。具体请求、生命周期和输出限制见 [本地任务服务协议](docs/task-service.md)。CodeStudio Next 与其他可信客户端可通过此接口接入。

Windows/Linux/macOS 可用 `scheduler-start --supervise` 或 RPC `scheduler/start` 的 `supervise: true` 显式启用有界崩溃监督；自启登记也可显式选择监督模式，默认仍直接运行 worker。能力分别通过 `worker_supervision`、`supervised_autostart` 声明；`supervisor_diagnostics` 声明持久监督记录、事件查询和只读进程身份观测。监督不会扩大任务原授权、预算或自动重放未知动作。

[机器可读协议](protocol/task-service.schema.json) 定义请求、响应、通知与 43 个方法的映射，协议版本保持 2。初始化返回本次进程的 `session_id`；`operations/get` 可查询当前操作和最近 64 个完成回执。重启后应查询持久任务和事件，不能把缓存缺失视为重新执行授权。错误提供稳定的类别和恢复建议，客户端无需解析错误消息。

[客户端示例](examples/task_client.py) 只依赖 Python 标准库，默认读取列表后退出。使用已有的专用目录并添加 `--demo`，可体验离线任务与完成事件（会写入 `CODESTUDIO_DEMO.txt`）：

```powershell
python examples/task_client.py --agent build/windows/x64/release/codestudio-agent.exe `
    --workspace D:\YourExistingDemoDirectory --demo
```

## 本地模型与 MCP 插件

对已启动的 Ollama、LM Studio 或其他 OpenAI 兼容本地服务：

```powershell
xmake run codestudio-agent models --base-url http://127.0.0.1:11434/v1
xmake run codestudio-agent model-check --model '实际模型名' `
    --base-url http://127.0.0.1:11434/v1
```

这两个命令仅连接字面回环地址，忽略云端端点和模型配置；默认不发送密钥。探测不执行模型提出的工具，不创建任务数据库。详情见 [本地模型](docs/local-models.md)。本地模型后续开发已按要求暂停，包括服务启动、模型下载和内嵌推理。

按 [version 2 示例清单](examples/mcp.example.json) 配置本机绝对 executable/脚本路径，再检查 MCP 工具。本地配置只接受 version 2，该版本号独立于 MCP 握手中的日期协议版本：

```powershell
xmake run codestudio-agent mcp-tools --workspace . `
    --mcp-config .\mcp.json --allow-mcp echo
```

`run` 加入相同的 `--mcp-config` 和 `--allow-mcp` 参数即可使用插件。插件调用进入同一 SQLite 动作账本，结果不明时停止继续执行。`mcp-tools` 会实际启动所授权的 stdio 进程或连接 HTTP 端点；stdio 默认 `none` 模式拥有当前用户权限。适配为自包含 executable 的插件可选择 `--sandbox appcontainer`，现有 Python 脚本示例不会自动打包进入沙箱。远程服务使用 [HTTP 示例清单](examples/mcp-http.example.json)，凭据与模型接口独立，默认要求 HTTPS。详见 [MCP 配置与边界](docs/mcp.md)。

## 状态和恢复

任务结果状态包括 `completed`、`needs_attention`、`failed`、`cancelled`、`exhausted`、`interrupted`；`waiting_approval` 表示等待构建决定，CLI 退出码为 3，任务尚未完成。动作的执行状态与效果状态分别记录；`unknown` 表示无法确认效果，不能当作“没有执行”。

```powershell
xmake run codestudio-agent recover --workspace D:\YourProject
```

恢复命令只在取得执行锁后操作：将仍在运行的任务标为中断，未派发动作标为未执行，已派发但无回执的动作标为结果不明。随后可从受支持的检查点继续同一任务：

```powershell
xmake run codestudio-agent resume <task-id> --workspace D:\YourProject
# 原任务的 --allow-write、--allow-process、--allow-mcp、--allow-build-proposals 和 --sandbox 模式需要再次提供。
```

`resume` 恢复完整对话与协议续传内容，复用已有回执。构建提案已获决定且执行结果明确后，它会将决定和结果作为原工具调用的回复交给模型；仍待审批、构建运行中、存在结果不明动作或配置/权限变化时拒绝继续。回注前还会重算完整提案摘要，并检查新增反馈能否在当前密钥下原样保留。合并审批后的续跑失败不撤销已提交决定或构建回执，需分别检查两个阶段的结果。`--max-turns` 按整个任务累计，耗尽后需要显式提高；不会后台自动追加预算。批准提案创建的独立构建任务没有模型检查点，不支持 `resume`；重试需要新提案。详情见 [检查点与续跑](docs/resume.md) 和 [构建提案](docs/build-proposals.md)。

需要在客户端退出后继续等待时，可用 `schedule-resume` 显式保存原授权，并用 `scheduler-start` 启动独立 worker。每个新提案仍需明确审批，调度不会增加模型预算或重放不明效果；成功手动续跑会取消该任务原有排队登记。worker 每次续跑按保存引用读取凭据，系统库轮换在下一个操作生效；环境引用仍使用启动环境，换值需从新环境重新启动；`scheduler-stop` 取消当前活动 job 并保留其他队列。完整限制见 [后台续跑](docs/scheduler.md)。

周期任务使用 `cron-create` 显式保存新任务模板，例如工作日 9 点检查项目：

```powershell
codestudio-agent cron-create --workspace D:\YourProject `
    --cron '0 9 * * 1-5' --timezone Asia/Shanghai --name '工作日项目检查' `
    --prompt '阅读 README.md，报告需要关注的问题。' `
    --api responses --base-url https://your-provider.example/v1 --model your-model `
    --max-turns 8 --max-runs 20
codestudio-agent scheduler-start --workspace D:\YourProject
codestudio-agent scheduler-autostart-enable --workspace D:\YourProject
```

`--max-runs 0` 表示持续重复，直到暂停或取消；每次触发有独立的模型预算及期限。`local` 时区在创建时固定为 IANA 名称；夏令时缺失分钟跳过，重复分钟分别对应两个 UTC 时刻。遗漏的空闲触发合并，上一轮运行或等待审批期间的触发跳过；阻断或未知效果会暂停计划。创建不请求模型，但配置 MCP 时会启动已授权服务器以冻结工具 schema；RPC `crons/create` 因此是异步登记。

Windows 自启采用当前用户登录触发；Task Scheduler 的 `Hidden` 只隐藏登记项，不保证控制台进程没有窗口。Linux 使用 systemd 用户单元和 `default.target.wants`，返回 `trigger: user_manager_start`，通常随首次登录启动用户管理器；管理器已运行时再登录不会重新触发。Linux 登记不 reload/start 当前管理器，也不改变 linger，在下一次管理器启动时生效。WSL 必须先启动发行版，此功能不会唤醒 WSL 或注册 Windows 登录任务。macOS 使用当前用户 LaunchAgent，返回 `current_user_logon`，登记在下次 GUI 登录时采用；只读检查系统禁用状态，既不 bootstrap/kickstart，也不修改系统禁用设置。

两平台自启默认执行 `scheduler-run`，可显式登记带有限重启次数的 `scheduler-supervise`：

```powershell
# 已有登记若模式或参数不同，先禁用，再登记新配置。
codestudio-agent scheduler-autostart-disable --workspace D:\YourProject
codestudio-agent scheduler-autostart-enable --workspace D:\YourProject --supervise `
    --max-restarts 3 --restart-delay-ms 1000
```

登记不立即启动进程、不保存账号密码或 API Key；Windows 的系统重启次数保持 0，Linux 保持 `Restart=no`，macOS 保持 `KeepAlive=false`，监督器自身不会被自动拉起。每次后续有效的平台触发启动新的监督生命周期，不能延长上一轮任务的原预算或权限。环境引用取自实际启动进程环境（Linux 为用户管理器环境），不能依赖启用命令所在终端的临时环境变量。任务也可绑定系统凭据引用；对应用户的凭据库必须在实际启动会话中可读取。可用 `scheduler-autostart-status` 查询已登记的 `supervised` 和监督限制；改变模式、轮询间隔或监督限制需先 disable 再 enable，停止当前 worker 仍使用 `scheduler-stop`。Linux 的运行时可用性、注销行为、配置路径和登记检查边界见 [systemd 用户管理器自启](docs/scheduler.md#linux-systemd-用户管理器自启)。

需要 worker 异常退出后自动恢复时，单独选择有界监督：

```powershell
codestudio-agent scheduler-start --workspace D:\YourProject --supervise `
    --max-restarts 3 --restart-delay-ms 1000
# 也可在当前终端运行监督循环。
codestudio-agent scheduler-supervise --workspace D:\YourProject
```

重启次数在监督进程生命周期内累计，默认最多 3 次；退避从 1000 毫秒开始倍增，最多 60000 毫秒。`scheduler-stop` 在运行与退避期间都阻止继续启动。监督器退出不会留下其子 worker；监督器自身不自动重启。`scheduler-status` 在最后 worker 状态旁返回 `supervisor`：已保存的阶段、重启次数、最后退出码、下一次重启时间，以及查询时的只读进程身份观测。历史阶段和心跳不能证明存活，身份匹配也不保证进程健康或未来继续运行。

```powershell
codestudio-agent scheduler-supervisor-status --workspace D:\YourProject
codestudio-agent scheduler-supervisor-events --workspace D:\YourProject --after 0 --limit 50
```

事件可按游标重读；每页最多 100 条，心跳不会产生事件。查询不启动进程，也不授予结束进程或重放任务的权限。完整字段与边界见 [监督状态与事件](docs/scheduler.md#监督状态与事件)；跨平台结果见 [验证记录](docs/validation.md)。

任务记录只接受 schema 7 状态库，以及 version 2 的检查点封装、执行绑定和内核状态。已初始化的 schema 6 数据库与其他不匹配版本均明确拒绝，不迁移，也不按任务隐藏旧记录。请使用新的 `--state` 路径或新工作区创建任务；运行时不会自动转换或删除状态库。

数据库采用 WAL、FULL synchronous 和事务。运行记录保留用户提示词、工具输入输出与文件内容，**目前不加密**；请按敏感项目资料保护数据库。应用对本次选中的密钥及既有 `CODESTUDIO_API_KEY` 做脱敏，并从子进程环境移除 `CODESTUDIO_API_KEY` 与 `OPENAI_API_KEY`，但不承诺识别工具输出中的所有秘密。自定义环境引用不会自动成为所有子进程的环境过滤规则；同用户未隔离程序也可能调用系统凭据 API，工具授权和 OS 隔离仍是独立边界。

文件工具拒绝绝对/越界路径、链接、Windows reparse point、ADS/设备路径，保护状态目录，并限制文本大小。替换前检查文件身份与修改信息。最终检查到替换之间仍存在竞态窗口；这是一套面向可信工作区的防误操作边界，不抵抗恶意同用户进程。进程工具的权限也不受文件工具路径规则限制。

## 测试

测试只使用临时目录、测试程序及本地 HTTP 服务，不需要云端账户：

```powershell
python -m venv build/contract-python
build/contract-python/Scripts/python.exe -m pip install -r tests/requirements.txt
$env:CODESTUDIO_CONTRACT_PYTHON = (Resolve-Path build/contract-python/Scripts/python.exe).Path
& $env:CODESTUDIO_CONTRACT_PYTHON tests/contract_schema.py
xmake build -a
xmake run agent-tests
python tests/provider_integration.py build/windows/x64/debug/provider-fixture.exe
python tests/cli_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/streaming_integration.py build/windows/x64/debug/provider-fixture.exe build/windows/x64/debug/codestudio-agent.exe
python tests/local_models_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/mcp_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/resume_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/credentials_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/scheduler_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/cron_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/autostart_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/sandbox_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/service_integration.py build/windows/x64/debug/codestudio-agent.exe
& $env:CODESTUDIO_CONTRACT_PYTHON tests/client_contract_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/build_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/xmake_build_integration.py build/windows/x64/debug/codestudio-agent.exe
python tests/build_proposals_integration.py build/windows/x64/debug/codestudio-agent.exe
```

`scripts/verify.ps1` 提供相同的 Windows 验证入口，支持 Debug/Release。`CODESTUDIO_CONTRACT_PYTHON` 指向安装了测试依赖的 Python；未指定时使用 `python`。JSON Schema 校验器仅用于开发测试，不是 Agent 运行依赖。契约集成测试可加 `--service-suite`，在原有服务场景上逐帧校验协议。测试覆盖权限拒绝、路径边界、读写、进程参数/输出/取消/后代清理、持久化、状态转换、重复调用、模型错误、密钥脱敏及实际强制退出后的恢复。

实际 XMake 隔离构建使用 [测试夹具](tests/fixtures/xmake-build/)，其中的项目和清单仅用于回归验证。

验证记录见 [验证结果](docs/validation.md)，协议实现说明见 [流式接口](docs/streaming-apis.md)。

## 目录与后续工作

```text
include/codestudio/   核心接口
src/core/            SQLite 记录、执行锁、任务循环
src/application/     CLI/本地服务共用执行入口与 stdio JSON-RPC
src/provider/        模型适配和离线演示
src/mcp/             stdio/HTTP 传输、握手、发现、参数检查与调用
src/tools/           参数校验、权限和文件工具
src/execution/       通用构建清单、工作区计划验证、隔离执行与产物发布
src/platform/        Windows / POSIX 进程后端
src/cli/             命令行入口
tests/               C++ 行为测试与本地 HTTP/CLI 集成测试
```

构建通过通用 v2 显式清单执行，清单与内部执行计划使用同一格式，宿主不生成工具专用命令或默认环境。通用宿主负责清单校验、授权、沙箱、资源限制、产物校验与账本。构建绑定和提案回执不包含插件标记；MCP 插件独立提供工具扩展能力，尚无外部 MCP 调用宿主隔离执行的接口。

Windows/Linux 工作区导入/导出和 CPU/内存限制已实现，后续仍需 OS 磁盘配额、工具链自动打包、macOS 后端与跨平台 CI。其他构建工具的清单仍需分别验证依赖和沙箱兼容性。CodeStudio Next 通过本地任务 API 复用核心。本地模型后续工作已按要求暂停。Windows/Linux 持久续跑、cron、各自平台自启、显式有界 worker 监督、持久监督诊断和模型凭据引用/系统存储已实现，自启也可显式选择监督模式；远程 MCP 的 Streamable HTTP 和显式 Bearer 凭据已实现；公开客户端 OAuth、插件精确版本锁定和本地/HTTPS 分发已实现；多代理、费用估算、任务数据库加密和托管插件市场尚未实现。具体依赖与验收条件见 [开发路线](docs/roadmap.md)。

本轮实现 **macOS arm64 基础运行与原生验证**，本机验收结果见 [验证记录](docs/validation.md)；已提供 macOS 原生 CI 作业定义，托管运行仍须单独确认。阶段 2 已加入 Keychain/后台调度/LaunchAgent 系统集成，真实登录验收单独记录；后续完成隔离后端和正式交付；Intel/Universal 包按需求扩展。四阶段边界见 [macOS 指南](docs/macos.md)。

MCP 扩展管理：[OAuth 登录与刷新](docs/mcp-oauth.md) · [插件版本锁定与分发](docs/plugins.md)。

完整插件包可在 Windows LPAC / Linux namespace 中运行，包文件只读、临时目录独立，并显式限制进程、内存和 CPU。见[插件隔离用法](docs/plugin-sandbox.md)。
