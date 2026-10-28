# CodeStudio Agent 后续路线

这是一份按依赖关系排列的实施路线，不是工期承诺。核心继续使用 C++26 + XMake；mcpp 等生态成熟后再评估迁移。

CodeStudio Agent 专注无界面的 Agent/Harness、CLI 和服务接口。桌面 UI 属于独立项目 **CodeStudio Next**，界面实现和技术选型由该项目负责；Agent 提供接入契约并执行权限、审批、隔离、恢复和调度规则。

| 阶段 | 交付内容 | 验收条件 | 状态 |
|---|---|---|---|
| 核心执行 | CLI、文件/进程工具、权限、SQLite 动作记录 | 失败不冒充成功，中断能保留结果不明的动作 | 已实现 |
| 模型协议 | Chat Completions / Responses / Messages，流式及非流式 | 分片、截断、取消和工具续传通过集成测试 | 已实现 |
| 本地模型接入 | 服务模型列表、实际工具调用能力探测 | 无需云端密钥；探测不执行模型提出的工具 | 已有基础；后续工作按用户要求暂停 |
| MCP 插件基础 | stdio、version 2 本地配置与显式授权、工具发现与调用 | 未授权不启动，插件超时/崩溃可回收，调用进入统一账本 | 已实现 |
| OS 隔离 | Windows LPAC、Linux namespace/seccomp/cgroup v2 后端与能力声明 | 真实文件/网络拒绝；平台不支持时拒绝请求，禁止静默回退 | 已实现普通单进程隔离与独立工作区构建模式；构建支持源码/工具链副本、进程树、CPU/内存限制和产物导出；验证见 validation.md |
| 通用构建清单 | 普通清单解析模块、薄工具入口、通用宿主执行模块 | 清单与执行计划统一使用 v2，显式步骤和环境 | 已实现；产品使用通用计划，XMake 配置仅用于测试夹具 |
| Agent 构建提案与审批 | 模型提交计划后暂停、宿主审批、独立隔离执行、决定与结果回注 | 未审批不执行；显式原授权续跑；重复决定和回注不重放 | 已实现 CLI 与 stdio 审批后显式续跑；提供客户端审批接口 |
| 持久化续跑 | 对话检查点、工具回执关联、配置/策略版本绑定 | 在模型/工具/数据库边界注入中断后恢复；未知副作用不自动重放 | 显式 resume 已实现；持久调度复用同一执行路径 |
| 后台续跑调度 | 显式保存原授权、独立 worker、延迟/退避、审批等待及取消 | 客户端退出不取消 worker；限制预算与尝试次数；安全恢复不重放工具 | 已实现 CLI / stdio 控制和 Windows/Linux 后台启动；显式有界 worker 监督已通过 Windows/Linux Debug、Release 验证；macOS 后台启动与有界监督已实现 |
| 监督诊断 | 持久阶段、重启次数、子 worker 退出回执、下一次重启时间、分页事件和进程身份观测 | 历史状态与当前观测分开；崩溃不伪造退出回执；查询不控制进程 | 已实现 CLI / stdio，只读查询共用机器契约；数据库使用 schema 7，协议保持 2 |
| Cron 周期任务 | 五字段表达式、固定 IANA 时区、独立任务与预算、遗漏合并、暂停/取消 | 重复触发不重复建任务；审批不重叠；未知效果暂停计划；DST 预览准确 | Windows/Linux/macOS 共用 CLI / stdio；`crons/create` 为异步登记，不使用系统 crontab |
| 平台自启 | Windows 当前用户登录、Linux systemd 用户管理器启动；固定 executable/工作区/状态库 | 不保存密码或密钥；登记不立即执行；不覆盖冲突；禁用不终止当前 worker | 三平台 enable/status/disable 支持显式监督模式；Linux 不修改 linger 或当前管理器；macOS LaunchAgent 登记已实现，真实登录触发另行验收 |
| 模型凭据 | 环境/系统引用、Windows Credential Manager、Linux Secret Service、macOS Keychain、CLI 设置/删除/状态 | 只持久化引用；每次操作解析并固定值；缺失/锁定拒绝且无明文回退；轮换不重放任务 | 已实现；服务只接受引用选项，凭据管理仅 CLI；schema 7、协议 2 保持不变，验证范围见 validation.md |
| 客户端接入契约 | 本地任务/事件服务、查询、流式文本、审批和调度接口 | 客户端与 CLI 使用相同任务语义，可取消、观察和接管 | stdio 服务、机器可读协议、结构化错误、会话操作查询与 Python 示例已实现；CodeStudio Next 的界面与接入工作在其项目中开展 |
| 本地模型管理 | 多服务配置、模型切换、服务生命周期、资源检测 | 已安装服务可诊断；下载/加载模型的成本和行为明确 | 按用户要求暂停 |
| 远程 MCP | Streamable HTTP、显式凭据引用、JSON/SSE、会话与 GET 恢复 | 冻结端点/授权；凭据隔离；断线不重发工具调用 | 已实现 Windows/Linux；平台限制见 mcp.md |
| 完整插件包隔离 | 只读包副本、私有依赖、独立 scratch、明确进程/内存/CPU 限制 | Windows/Linux 实际加载多文件包，宿主/网络/包写入拒绝，续跑权限不扩张 | 已实现；验证范围见 validation.md |
| 隔离资源生命周期 | 私有归属登记、租约、出生身份、扫描/预览/显式回收 | 活跃或身份不明时拒绝；崩溃回收不重放动作 | 接口与平台实现见 sandbox-resources.md；实际验证以本轮回执为准 |
| 插件生态 | OAuth 登录/刷新、精确版本锁定、可验证包与本地/HTTPS 分发 | 认证与内容绑定、显式换锁，活动调用不被静默替换 | 已实现；验证范围见 validation.md；动态客户端注册、签名信任与托管市场留待后续 |
| 构建与交付 | 统一原生验证、机器回执、CI 定义、标准包、用户级安装/升级/卸载 | 失败不标通过；文件被修改时保留数据；安装后程序实际验证 | 工具已实现；实际验证范围以 delivery.md 和回执为准，干净机器及真实登录仍需单独验收 |

## 当前阶段：macOS 原生适配

本轮已加入 **macOS arm64 基础运行与原生验证** 的实现，原生 Debug/Release 验收通过。Apple Silicon 优先；详细工具链、能力边界和四阶段验收条件见 [macOS 指南](macos.md)，实际结论见 [验证记录](validation.md)。

| 顺序 | 交付范围 | 验收目标 | 当前状态 |
|---|---|---|---|
| 1. 基础运行 | C++26/XMake 工具链与标准库、依赖、进程执行与出生身份、CLI/stdio 服务、模型协议、MCP、任务恢复、原生 CI | arm64 Debug/Release 原生构建和基础套件通过，产生准确记录能力与跳过项的回执 | 已实现，本机原生验收通过；托管 CI 待实际运行 |
| 2. 系统集成 | Keychain、后台 worker、有界监督、cron、LaunchAgent 当前用户登录自启 | 凭据轮换、后台续跑、监督与真实登录启动通过验证，权限和预算不扩张 | 代码已实现；本机验证及真实登录证据见 validation.md |
| 3. 隔离能力 | 先验证隔离技术方案，再实现插件/工作区隔离、资源限制、归属登记和崩溃回收 | 文件/网络限制、取消、资源回收及不重放未知效果通过实际验证 | 安全文件准备层已实现；原生资源与生命周期保证存在实测缺口，保持原生 macOS，完整隔离暂不启用 |
| 4. 正式交付 | 用户级安装/升级/卸载、签名、公证、干净 Mac 验收；按需求增加 Intel 或 Universal 包 | 已声明架构和能力在独立 Mac 上验收，交付产物与验证记录对应 | 待目标能力完成 |

阶段 1 即接入 macOS 原生 CI，阶段 4 完成正式分发。现有 Windows/Linux CI 定义和本地回执不能代替 macOS 验证；实际托管运行结果以对应作业回执为准。Mac 主机或 macOS runner 是原生验收前提，Windows/WSL 可用于准备适配代码。

macOS 隔离后端不能直接复用 Windows Job/LPAC 或 Linux namespace/cgroup，需单独确定支持范围。在阶段 3 完成前，缺失的隔离能力必须明确声明并拒绝相应请求。桌面 UI 继续由 CodeStudio Next 实现，本地模型后续开发继续暂停。

## 下一步具体边界

Windows 首版使用 `--sandbox appcontainer` 选择 LPAC；默认 `none` 仍以当前用户权限运行。普通命令与未锁定的 stdio MCP 共用启动策略：零 capabilities、单 executable 暂存、独立 scratch，无工作区导入和输出回写。Job Object 限定一个活跃进程、禁止直接后代运行并负责生命周期回收。隔离边界和当前机器上的真实拒绝结果分别见 [沙箱说明](sandbox.md) 与 [验证记录](validation.md)，不能由构建能力声明推导实际验证通过。

[工作区隔离构建](workspace-builds.md) 已提供固定清单、只读工具链副本、可写源码副本、多进程 Job、CPU/内存限额及原子导出。后续隔离工作包括 OS 磁盘配额、工具链自动打包、缓存复用、macOS 后端与跨平台 CI。LPAC 仍受系统资源 ACL 影响，不等于虚拟机；锁定 MCP 插件可使用[完整包隔离](plugin-sandbox.md)，其进程权限与资源限额独立绑定；不能承诺任意不可信插件均已达到生产安全要求。

构建流程由普通模块实现：`normalize_build_manifest` 接受 v2 清单并生成同版本计划，`WorkspaceBuild` 对接工具及恢复绑定，`WorkspaceExecution` 负责宿主验证和隔离执行。工具步骤和全部所需环境由清单显式声明，XMake 命令及私有缓存配置仅保存在 [测试夹具](../tests/fixtures/xmake-build/) 中。绑定和提案回执不包含插件标记。状态库只接受 schema 7，检查点封装、绑定和内核状态只接受 version 2；其他版本拒绝读取和执行，需要在新状态库或新工作区创建任务，不迁移旧数据。后续可按实际需求增加 CMake、Cargo 或 mcpp 的兼容性测试；需要工具链发现或诊断解析等实际能力时，再评估专用扩展。第三方 MCP 工具继续独立工作，但目前不能直接调用宿主隔离执行，也未开放动态库 ABI 或运行时加载/卸载。

续跑使用版本化的对话检查点和配置/策略绑定，权限由调用方再次提供。[后台续跑](scheduler.md) 已支持把这次明确的原授权登记到持久队列，并由独立 worker 按延迟、期限、尝试次数和原模型预算运行。成功手动续跑取代原排队登记。worker 重启时只恢复自己遗留的安全任务；其他前台中断仍需显式 `recover`，未知效果始终阻止重放。凭据引用随任务绑定保存；系统库在每次任务/续跑开始时重新读取，当前操作固定使用取得的值。环境引用仍受启动环境限制，轮换这种来源需要重启进程。Windows/Linux/macOS 已支持独立后台启动，Linux 脱离终端但仍受原 cgroup、用户会话和 WSL 发行版生命周期管理。`scheduler-supervise` 和显式 `scheduler-start --supervise` 持有子进程身份、有限重启次数、指数退避和停止代数校验；跨平台验证结果见 [验证记录](validation.md)。监督器自身不自动拉起，macOS 后台启动与有界监督已实现。

[监督状态与事件](scheduler.md#监督状态与事件) 已提供最新监督运行快照和独立事件游标。`scheduler-status` 在历史 worker 状态旁返回 `supervisor`；专门的 CLI / RPC 查询可读取持久阶段、次数、最近子进程退出、退避时刻，并进行只读出生身份观测。心跳不会追加事件，旧的非终态记录可由新准入运行标记为 superseded；它不表示实际观察到旧子进程退出。身份匹配只描述查询时刻，不能保证健康、未来存活或授权按 PID 控制进程。已初始化的 schema 6 数据库明确拒绝，不提供迁移；新数据库使用 schema 7，任务协议仍为 2。

周期计划使用 `cron-create` 冻结提示词、配置、权限和零轮次检查点，每次触发生成新任务与完整独立预算。漏掉的空闲时刻合并为最新一次，上一轮仍活动时跳过到期时刻；审批和人工接管也不能造成同计划重叠。遇到阻断或未知效果暂停计划。`local` 在创建时解析为固定 IANA 时区，夏令时跳过缺失分钟、分别考虑回拨重复分钟。Windows 当前用户登录自启和 Linux systemd 用户管理器自启均已提供 enable/status/disable，默认运行 `scheduler-run`，可通过 `scheduler-autostart-enable --supervise` 登记固定限制的 `scheduler-supervise`。改变模式、轮询间隔或监督限制必须先 disable 再 enable；登记不立即启动、不保存凭据，状态返回已核验的实际配置。Windows 的系统 `restart_count` 固定为 0，Linux 固定 `Restart=no`，监督器本身不自动重启；新的平台触发可开始新监督生命周期，不重置任务权限或预算。Linux 精确返回 `user_manager_start`，不调整 linger 或 reload 当前管理器，默认在下一次用户管理器启动时采用配置；WSL 发行版必须先被启动。Windows Task Scheduler 的 Hidden 不保证控制台进程没有窗口。macOS 使用当前用户 LaunchAgent，固定 `RunAtLoad=true`、`KeepAlive=false`，登记不立即执行，禁用不终止已有 worker；真实登录触发单独验收；计划管理界面由 CodeStudio Next 通过已有接口实现。

[构建提案](build-proposals.md) 使模型提出完整 v2 清单后暂停为 `waiting_approval`，由宿主通过精确摘要批准或拒绝。默认审批只处理决定及独立构建；显式 `resume`、携带原授权的 `--continue-task` / `continue_options`，或已单独登记的 worker，可以将决定和明确的执行回执交给原工具调用并继续会话。消费事件与检查点推进原子提交，重复审批不会推进后续任务。后台授权不包含自动审批，每个新提案仍需明确决定。Agent 提供完整清单、步骤、输入/挂载、资源限制、队列与继续执行接口，供 CodeStudio Next 等可信客户端展示并收集用户决定；摘要仍不代表源码及工具链的历史字节快照。

当前 [本地任务服务](task-service.md) 已提供 stdio JSON-RPC、查询分页、事件重放、流式文本、启动/续跑/取消、持久队列、cron、worker、监督诊断和平台自启控制，并共用 CLI 执行入口。CodeStudio Next 作为独立客户端接入，Agent 继续负责执行侧的授权校验和恢复约束。[机器可读协议](../protocol/task-service.schema.json)、结构化错误和有界会话回执查询明确了断连后的处理规则；请求 ID 不是幂等键，临时文本不能断线重放。`worker_supervision`、`supervised_autostart`、`supervisor_diagnostics` 对应的显式监督、监督自启和诊断接口均纳入 43 个方法的契约。[凭据管理](credentials.md) 已提供环境/系统引用、当前用户系统存储、CLI 管理及操作边界轮换；本轮未增加凭据管理 RPC。Streamable HTTP MCP 已提供独立 Bearer 引用和有界 GET 响应恢复，服务通过 `mcp_streamable_http` 声明能力。统一验证入口、Windows/Linux CI 定义及用户级安装交付工具已提供；macOS 阶段 1 已扩展原生验证与 CI 定义，实际通过范围以回执为准，后续阶段逐步覆盖系统集成、隔离和正式交付。各平台仍需按实际 runner 日志及独立干净机器上的真实登录结果验收。OAuth 登录与插件包通过异步操作接入，定义见 [OAuth](mcp-oauth.md)、[插件分发](plugins.md)；托管市场、发布者签名和动态客户端注册后续设计。跨重启请求幂等性须单独设计持久授权与动作关联。

本地模型分成两层：当前通过本地 HTTP 服务推理；后续管理服务、模型资源与配置。嵌入 llama.cpp 是可选方向，尚未决定；不会因为核心使用 C++ 就默认绑定某个推理引擎。
