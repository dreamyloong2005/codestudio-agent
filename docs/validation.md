# CodeStudio Agent 0.1.0 验证记录

本记录描述当前格式的回归范围和本轮验证状态。项目自身使用 C++26 + XMake；工作区隔离执行接受通用步骤清单，XMake 命令只属于真实构建测试夹具。当前验证目标为 Windows x64 / clang-cl、Arch Linux x86-64 / GCC 16（WSL2）和新增的 macOS arm64 / LLVM 22。各平台的 Debug 与 Release 顺序执行；Linux 使用独立原生目录，不覆盖 Windows XMake 缓存。

## 当前格式约定

| 对象 | 当前版本或标识 |
|---|---|
| SQLite 状态库 | schema 7 |
| 检查点封装、绑定、内核状态 | version 2 |
| 构建清单、执行计划 | version 2，无版本转换 |
| 构建绑定、提案回执 | version 2，无 `plugin` 标记 |
| 构建后端 | Windows `windows-lpac-build-v2` / Linux `linux-namespace-build-v2` |
| 沙箱策略、`build_proposals` 授权 | version 2 |
| 本地 MCP 配置 | version 2 |
| stdio 服务初始化 | `protocol_version: 2` |
| 事件、产物 `manifest.json` | `schema_version: 2` |

状态读取和写入均要求当前数据库格式；其他已初始化版本在修改数据库设置前被拒绝，不迁移数据库，也不逐任务过滤历史。需要新建任务时使用空的状态路径或新工作区。`none` 沙箱策略也明确保存为 `{"version":2,"mode":"none"}`。本地 MCP 配置版本独立于 MCP 日期协议；模型服务 URL 中的 `/v1` 是供应商现行接口路径。

## macOS 阶段 4：包、安装与发布验证

新增 macOS arm64 可搬移包，带私有 libc++/libc++abi 与许可证，去除构建机运行库路径，并在副本上重定位/签名。原构建、重定位后包、最终签名及 DMG 的字节摘要分别记录，不将打包后新字节冒充原生测试的原始可执行文件。

用户级安装增加 Darwin 运行镜像身份观察、APFS 路径别名归一、继承 ACL 收敛及 quarantine 属性保留。临时安装验收运行实际 doctor、离线任务、stdio 服务、后台 worker、IANA 时区、升级/卸载与数据保留，并覆盖修改/链接/活跃进程拒绝和中途失败。包内独立验收脚本只需要 Python 标准库，可输出其实际运行范围；不自动证明干净机器或真实登录。

签名公证工具使用显式 Developer ID 与既有 notarytool profile，签名后更新所有摘要，再创建/签名 DMG、提交公证、装订和 Gatekeeper 检查。上传结果未知时保留状态并拒绝盲目重传；只有实际成功才能记为已公证。开发 ad-hoc 路径仅用于本地测试，不计为正式发布成功。完整 Debug/Release、包与安装、签名公证和外部验收状态分别保存在[阶段 4 记录](../research/macos-stage4/README.md)；此处不由工具实现推导任何未执行的正式验收结论。

## macOS 阶段 3：文件准备层与隔离方案验证

本轮实现 Darwin 安全工作区导入、计量、原子产物导出和自有目录清理。原生测试新增普通/硬/符号链接、FIFO、保护路径、大小写/Unicode 别名、APFS firmlink 根重叠及缺失保护目录、复制限额、取消、SHA256 清单及真实 ACL 继承场景；可执行文件及 runtime 仍使用阶段 2 工具链。完整 Debug/Release 回归结果和源文件指纹见 [阶段 3 机器回执](../research/macos-stage3/verification.json)。文件准备层通过不等于隔离执行通过。受保护目录的祖先判定同时修正 Windows/Linux 的同形逻辑，通用回归已加入；本轮实际运行仅在 macOS，不增加其它平台通过结论。

Seatbelt 和资源接口另有真实小规模探针，源代码、脚本、运行环境、差分结果和已知限制保存在 [阶段 3 原型目录](../research/macos-stage3/README.md)。原型表明文件/网络拒绝可实现，但当前非特权原生路径不满足已有整组内存、CPU 百分比、进程数和一般进程树收敛契约。私有 API、单进程 guardian、轮询后终止或 UID 总量都不被记为等价的内核执行容器。`os_sandbox`、`plugin_sandbox`、`workspace_build` 和资源回收继续明确拒绝；**阶段 3 整体验收尚未完成**，不能将原型案例成功或安全复制测试计为完整后端成功。

阶段 2 的真实登录验收仍单列 pending；其原始已验证 executable 已复制到独立的 `build/macos-stage2-login-runtime/`，登录 fixture 使用该固定副本，后续开发构建不会替换其字节。更新后的准备回执见 [阶段 2 登录准备](../research/macos-stage2/login-acceptance-prepared.json)。

## macOS arm64 系统集成（阶段 2）

阶段 2 新增非交互 Keychain、独立 OAuth 记录与刷新互斥、Darwin 后台启动/有界监督、当前用户 LaunchAgent 登记及真实后台 cron 联测。统一入口对 Debug/Release 运行完整的当前可用套件，逐项成功、失败、跳过、源码/产物/运行库摘要见 [阶段 2 机器回执](../research/macos-stage2/verification.json)。环境沿用下述阶段 1 的本机 macOS 26.5.2 arm64；本轮不推导 Windows/Linux、macOS 15.0 或 Intel 的新验收结论。

Keychain 原生夹具覆盖缺失、设置、读取、轮换、锁定后读写/删除拒绝、归属冲突和模型/OAuth 域隔离，专用临时库清理前后核对默认库与搜索列表。CLI/OAuth 使用随机自有条目并删除，覆盖操作边界轮换、刷新串行、刷新派发后的崩溃阻断、取消与脱敏。后台生命周期覆盖客户端退出、无继承管道/锁、父身份不符、父退出发生在 exec 前后、有限重启与退避、停止代数及诊断。cron 联测覆盖时区、遗漏合并、审批不重叠、每轮独立预算和未知效果阻断，原有 DST 缺失/重复时刻探针继续运行。

LaunchAgent 自动测试覆盖固定 plist、字面参数、路径/归属检查、不覆盖冲突、无即时启动、`KeepAlive=false`、禁用不停止运行 worker。**真实注销/登录触发仍待完成，因此阶段 2 的整体验收尚未完成。** 已提供 [可续接登录探针](../scripts/verify-macos-login.py)，步骤见 [macOS 指南](macos.md)。静态登记和普通后台启动结果不替代该证据。阶段 3 的隔离插件/资源、真实隔离构建及阶段 4 的交付套件继续明确跳过；不会降级为宿主隔离执行。托管 macOS CI 的实际执行同样未由本机回执证明。

## macOS arm64 基础运行（2026-10-28）

阶段 1 代码与统一验证入口已加入，**本机 Debug/Release 原生验收均通过**。本轮环境为 macOS 26.5.2 arm64、SDK 26.5、LLVM 22.1.8、XMake 3.0.9（HEAD `2b184e178`）、Python 3.13.14。部署目标为 macOS 15.0，本轮仅验证 26.5.2。专用 libc++/libc++abi 22.1.8 由 `scripts/build-macos-libcxx.py` 启用实验性时区数据库并绑定系统 IANA 数据；系统 Apple libc++ 和官方 LLVM 包附带的运行库均不满足所需时区功能。依赖为 SQLite 3.50.4、nlohmann JSON 3.12.0、静态 OpenSSL 3.6.5 和系统 libcurl 8.7.1（SecureTransport）。详细复现命令见 [macOS 指南](macos.md)。

新增覆盖包括 C++26/格式化/时区 DST 运行探针、Darwin posix_spawn 的参数/环境/继承描述符与信号、超时取消、启动 UUID/出生时间身份、清单读取的链接/类型/大小与变动拒绝，以及 doctor/服务的平台能力声明。CLI、三种模型协议、流式、stdio/HTTP MCP、任务状态与恢复、共享审批决定、前台队列/cron 与协议契约继续使用本地夹具验证。

Keychain/OAuth 原生存储与刷新互斥、独立后台启动与监督、LaunchAgent、OS 沙箱、隔离工作区执行、资源回收、安装/签名公证均不属于本轮通过范围。不支持的完整套件与套件内场景分别记录为跳过；额外 CLI 回归检查这些请求明确失败、无环境密钥回退、无隔离降级。macOS 自托管 CI 作业仅完成定义，本记录不声称托管运行通过。

每个配置通过 22 组原生测试、1 个工具链运行探针和 280 项 Python 测试；Python 共登记 291 项，其中 11 项因对应原生功能未实现而跳过。8 个尚不支持的完整套件单独列入 `unsupported_suites`，不计入已通过套件；18 个实际运行套件均通过。构建、源码、产物及运行库摘要、逐套件日志见 [macOS 机器回执](../research/macos-stage1/verification.json)，详细原始回执由 `scripts/verify.py` 生成并保存在同目录。

首轮发现并修复了 Apple/LLVM 预编译 libc++ 缺少时区、Darwin 测试夹具误用 Linux 头文件、临时目录 `/var` 符号链接、沙箱测试误调用已明确不支持的工作区复制，以及新增 CLI 测试参数/模块导入问题；HTTP MCP 中真实隔离构建场景明确跳过，其余通信测试继续执行。原生测试还覆盖受保护目录/缺失 sidecar 的大小写与 Unicode 别名、关闭 stdin 和 RPC 管道后的 SIGPIPE、子进程退出身份。失败尝试保留在本机 `build/verification/macosx/`，不计入最终通过结果。

## 沙箱资源生命周期与崩溃回收（2026-10-25）

普通进程、插件包和工作区构建的 OS 沙箱建立当前用户私有的格式 2 归属登记，记录宿主出生身份、租约、资源原生身份和可用时的任务关联。CLI 提供只读分页、单 ID 预览与显式回收，任务服务共享同一实现。完整接口和恢复边界见 [资源回收文档](sandbox-resources.md)。

回收必须重新确认宿主已退出、租约空闲、Job/cgroup 无成员及路径身份一致；不杀进程、不修改 SQLite 或检查点、不重放未知效果。测试使用本项目原生夹具与私有登记目录，覆盖真实宿主强杀、活动拒绝、只读预览、分页、并发、路径替换、硬链接、符号链接/junction、PID 复用、取消、部分失败重试以及 CLI/服务契约。构建和审批崩溃测试在回收后比较完整数据库转储，保证动作状态不变。

初轮失败包括 Windows 链接器被历史挂起测试进程占用、活动锁文件不能读取的测试假设，以及新增保护路径在任务恢复绑定两侧不一致。失败日志与修复后的结果分别保留。进程核对不能只用 `ExecutablePath`：该字段可能为空；本轮对已确认的旧夹具核对命令行与创建 FILETIME，再使用同一进程句柄关闭并确认退出。

最终源码摘要、Windows/Linux Debug/Release 各套件计数、跳过原因与实际归档安装结果以 [本轮验证目录](../research/resource-lifecycle/) 的 `verification.json`、`delivery.json` 为准。`final-static.json` 区分本轮新增残留与历史未登记残留，不把前次清理失败隐藏为零残留。此前自动审批拒绝删除的夹具仍保留，不由新回收器接管。

两处崩溃窗口明确拒绝推断归属：资源已创建但原生身份尚未持久记录，以及外部资源全部清空后、最终登记文件删除到一半。前者可能保留资源，后者可能留下 `invalid` 登记残片。测试不代表电源故障、恶意同用户宿主、干净机器或真实登录/重启验收；没有永久安装、自启启用、在线推理或外部发布。

## 完整插件包隔离（2026-10-25）

锁定的 stdio 插件在明确选择 OS 沙箱后，将声明的程序、私有依赖和配置按冻结清单复制到独立只读目录；`${plugin}` 参数指向该隔离副本，临时文件进入本次独立可写目录。Windows 使用 LPAC/Job，Linux 使用 namespace/seccomp/cgroup v2；普通 `command` 配置仍采用原单程序路径。新增通用进程包模块负责持有清单、参数及摘要，不在平台后端解析插件协议。用法与限制见 [插件包](plugins.md)、[沙箱](sandbox.md) 和 [Linux](linux.md)。

每个插件的 `sandbox_limits` 明确绑定进程、内存和 CPU 预算；默认单进程。Linux 的任务数还包括线程，并额外预留两个 Bubblewrap 监督槽。新的 `isolated-process-package` 执行绑定使旧单程序沙箱检查点不能无声取得整包执行能力；包身份或资源授权变化时拒绝续跑。安装目录保持原样，包加载失败、取消或超时不降级到宿主执行；已派发工具仍按原未知效果规则阻止重放。

Debug 专项联调已分别通过两端 17 项插件隔离、22 组原生测试和 13 项静态 Schema。新增真实共享库的传递依赖与依赖缺失拒绝：Windows 的传递 DLL 放在入口程序同目录，Linux 私有库保留在嵌套目录。原生路径校验拒绝 Windows 带根但无盘符的路径及非规范分隔符。初轮 TEMP 别名、DLL 布局和路径拒绝失败回执独立保留，不计入通过结果。最终四配置的源码摘要、逐套件结果及跳过原因以 [本轮证据目录](../research/plugin-isolation/) 中的 `verification.json` 为准。

集成覆盖真实私有 DLL/ELF 共享库及嵌套配置加载、包只读与 scratch 可写、宿主工作区和网络拒绝、凭据环境不继承、每次全新临时目录、默认子进程拒绝及显式预算下派生、Job/cgroup 限额、超时/服务取消后的子树回收、篡改/链接/无效依赖拒绝、未授权不加载，以及权限和执行绑定变更后的续跑拒绝。原生接口测试另检查调用者 JSON/参数销毁后的冻结状态、同大小篡改、文件模式改变、路径冲突、硬链接、只复制声明文件及取消前不复制。

测试使用本项目构建的原生夹具、临时包和模拟模型，不代表任意第三方插件或完整 Python/Node 发行包都已兼容。Linux 入口要求原生 ELF，脚本须显式提供包内解释器；包内 ELF basename 必须唯一，系统运行库仍来自受信系统目录且未被版本锁冻结。只读隔离副本不抵御恶意同 UID 宿主进程、管理员或内核攻击。未实现 OS 磁盘配额、插件宿主工作区挂载或网络放行；本轮没有真实账号、在线推理、永久安装/自启、签名或外部发布验收。

最终交付检查源码及原生镜像一致、Debug/Release 独立成功回执、实际归档安装验证，以及进程、临时目录、AppContainer profile、cgroup 和测试凭据清理。分别查看本轮证据目录中的 `delivery.json` 与 `final-static.json`，不得把交付字节验证等同于资源全部清理。初轮原生测试留下的一份已核实临时夹具目录因自动审批拒绝删除而保留，见 [清理阻断记录](../research/plugin-isolation/cleanup-blocked.json)；最终资源检查仍会报告它，不将其过滤为成功。

## MCP OAuth 与插件版本锁定、分发（2026-10-25）

新增公开客户端 OAuth 发现、PKCE 登录、系统存储、状态/退出登录和刷新；插件支持确定性包、显式 SHA256 本地/HTTPS 安装、精确版本锁及冻结加载。两者共用 CLI 与异步任务服务，服务契约新增八个方法和授权链接通知，仍为协议 2、数据库 schema 7。使用与限制见 [OAuth](mcp-oauth.md) 和 [插件分发](plugins.md)。

最终范围以 [四配置验证汇总](../research/mcp-extensions/verification.json)、[实际归档与安装交付](../research/mcp-extensions/delivery.json)、[资源清理](../research/mcp-extensions/final-static.json) 为准。完整报告记录源文件与产物摘要、22 个原生测试组、各 Python 套件计数与跳过原因。使用统一验证入口；Linux 使用独立原生镜像、临时 cgroup 及私有真实 Secret Service，Windows 使用随机命名的测试凭据。交付归档从同一 Release 报告打包两次并比较字节，再从真实归档解压验证临时安装。

新增 OAuth 26 项覆盖元数据/resource/issuer 校验、PKCE/state/回调、超时/取消、令牌类型/scope/记录上限、原生命名空间隔离、并发刷新、进程崩溃后的禁止重放、配置变更、401 未知效果、CLI/服务通知、脱敏和调度预检。插件 23 项覆盖确定性打包、路径/链接/完整文件集合、摘要/版本/平台/执行模式、URL 下载拒绝与取消、侧版本安装、换锁与旧任务绑定、显式工具授权、篡改拒绝及 CLI/服务行为。

审查回归在修复前实际复现了授权链接回显已有令牌、参数/续跑校验前刷新、多连接后置失败前刷新、刷新后 cron 元数据持久化以及前序 MCP 进程修改后续插件的问题。日志以 `*-red.log` 保存；修复后的最终套件检查相同行为。最初 OAuth 测试清理遗漏的 kill 方法和管道关闭已补齐；初始失败回执单独保留，不计入通过结果。

所有身份服务、模型响应和下载服务均为自有临时夹具；没有真实账号登录、外部插件执行或发布、在线/本地推理、托管市场、签名信任、真实登录/重启或永久安装。OAuth 当前使用预注册公开客户端，系统记录最多 2048 字节；Linux 刷新锁限同一网络命名空间；插件摘要目录不是 OS 只读挂载，不能保证抵抗恶意同用户检查后替换。Windows 外层 Job 的后台独立启动成功场景保留跳过，不由其他平台结果替代。

## 前次构建与安装交付验证（2026-10-25）

本轮统一入口为 [scripts/verify.py](../scripts/verify.py)，要求与实际版本、源码摘要、执行文件摘要、完整步骤及未选/跳过套件一并进入独立回执。Windows/Linux 的 Debug、Release 顺序构建；标准发布包由 [package.py](../scripts/package.py) 创建，安装由 [install.py](../scripts/install.py) 管理。用法与故障恢复边界见 [交付文档](delivery.md)。

最终运行结果以 [本轮汇总](../research/delivery/verification.json)、[包交付回执](../research/delivery/delivery.json) 和 [最终资源检查](../research/delivery/final-static.json) 为准；对应原始统一报告和日志位于同目录的各平台子目录。早期失败或预检报告保留独立路径，不计作最终通过。源码/二进制在测试过程中改变会拒绝产生成功回执；相同构建目录的验证、相同目标的打包及安装分别有排他锁。

新增包测试覆盖确定性归档、路径/链接/所有权拒绝、绑定 Release 二进制及并发输出锁；验证入口测试覆盖假阳性输出、零测试、失败预检、超时、并发、部分报告与源/二进制变化。安装测试在临时用户前缀中使用实际包：doctor、离线写入、stdio 服务、环境凭据、升级/降级拒绝、文件篡改和额外数据保护、运行中拒绝替换、卸载保留外部工作区，以及后台启动/停止。故障注入检查复制中断与部分卸载时准确保留路径，不把不完整目录伪装成已恢复安装。

当前机器上的测试不等于干净系统或真实登录验收。Windows 外层 Job 的独立后台启动拒绝分支、Linux WSL 用户管理器和现有依赖环境都必须保留其限制。没有运行真实注销/登录、重启、CI 托管作业、线上发布或签名。CI YAML 已交付，runner 仍需部署；需要真实登录/干净机器时按照 delivery.md 的独立步骤采集验收记录。

## 前次远程 MCP 验证（2026-10-25）

实现 Streamable HTTP MCP 客户端，复用 stdio 的协议校验、工具授权及执行账本。新增冻结清单、独立 Bearer 凭据引用、JSON/SSE 响应、会话/版本头、取消通知、有界 GET 响应恢复和服务能力声明。状态库仍为 schema 7，各本地协议格式保持 version 2。

Windows x64 / clang-cl 与 Arch Linux x86-64 / GCC（WSL2）的 Debug、Release 均完成构建和相关回归：

| 检查 | Windows Debug / Release | Linux Debug / Release |
|---|---|---|
| C++ 原生行为 | 各 22 组通过 | 各 22 组通过 |
| 静态 Schema | 各 12 项通过 | 各 12 项通过 |
| 新增远程 MCP | 各 39 项通过 | 各 39 项通过 |
| 相关 Python 集成合计（含远程 MCP，不含静态 Schema） | 各 254 项通过、7 项跳过 | 各 237 项通过、26 项跳过 |

精确分项见 [验证回执](../research/remote-mcp/verification.json)。[执行脚本](../research/remote-mcp/verify.py) 与 [汇总脚本](../research/remote-mcp/summarize.py) 保留可重复命令；日志在同目录。已有模型协议、流式、CLI、stdio MCP、凭据、检查点、调度、监督、cron、自启、审批、服务及客户端契约都纳入本轮相关回归。Windows 跳过项来自 Linux 专属场景及外层 Job；Linux 跳过 Windows 专属自启项及原 25 项 Windows LPAC 审批套件。新的远程 MCP 审批脱敏测试独立在两平台实际执行隔离构建。

远程测试通过临时 HTTP 服务检查真实请求与 SQLite 回执：JSON/SSE 分片、会话与协商头、无状态服务、DELETE 405、独立/匿名凭据、无重定向、完整配置预检、GET 游标恢复/去重/重试期限、401/404、超时/取消、工具列表变化、schema/结果拒绝、凭据流式脱敏与轮换。明确检查不重发 `tools/call`，未知效果阻止显式 resume。审批测试修改或删除当前清单后仍使用源任务保存的引用脱敏；后台 worker 使用新凭据继续模型会话，不重复已经完成的工具。

原生测试另检查冻结计划不重新读取已被替换的清单，以及后续远程凭据缺失/无效时不得先启动较早的 stdio 进程。只读代码检查发现的派发前取消边界已增加原生发送标记：未到达原 POST 发送边界不发取消通知。

排查记录保留初始 [Windows 夹具失败](../research/remote-mcp/windows-release-mcp-http-initial.log)、[取消契约断言](../research/remote-mcp/windows-release-mcp-http-contract-failure.log)和 [Linux 原生超时失败](../research/remote-mcp/linux-release-mcp-http-timeout-initial.log)。夹具原先遗漏客户端 ping，并错误要求 run 预检完全不创建空数据库；现检查无任务/动作。取消后未知效果优先产生 `needs_attention`，初始化失败则尚无任务。Linux 单项测试实际复现原生计时器略早于 steady-clock 期限时遗漏取消通知，修复为独立识别原生超时，随后四配置完整远程套件通过。Release 在最终传输修补后重建并补跑原生/远程套件；不受修补影响的相关套件日志保留，重复案例只计一次。

本轮没有真实供应商认证、第三方 HTTPS 服务互通、OAuth、在线/本地推理、登录或重启验证。HTTP 网络测试均为合成 loopback 服务；HTTPS 使用原生证书校验且不关闭验证。404 后要求显式新加载，不自动重新初始化；Windows 仅支持 ASCII SSE 游标；未实现 OAuth 登录/刷新、旧双端点传输或独立长期 GET 订阅。不能将本轮实现描述为所有 MCP 可选能力或完整 OAuth 规范实现。具体限制见 [MCP 文档](mcp.md)。

Linux 交付保留原产物名称，加入远程清单与相关配置文档。镜像源码/测试/文档、归档字节/权限/SHA256 和交付程序能力见 [交付回执](../research/remote-mcp/delivery.json)。两平台恢复 Release；进程、自启登记、测试凭据、临时 cgroup/隔离目录/密钥环、工作区数据库及文档链接检查见 [最终静态回执](../research/remote-mcp/final-static.json)。

## 前次凭据管理与轮换验证（2026-10-25）

新增 `env:VARIABLE` / `system:identifier` 凭据引用、Windows Credential Manager / Linux Secret Service 原生存储、CLI 设置/查询/删除及模型操作的 `credential` 选项。密钥在每次任务、续跑或批准构建开始时读取并固定；绑定只保存引用，轮换不改变原授权、预算或重放规则。数据库仍为 schema 7、协议和检查点仍为 version 2；缺少凭据引用的旧模型检查点拒绝继续，不提供补值或转换。

Windows x64 / clang-cl 与 Arch Linux x86-64 / GCC（WSL2）的 Debug、Release 全部完成构建及本轮相关回归：

| 检查 | Windows Debug / Release | Linux Debug / Release |
|---|---|---|
| C++ 原生行为测试 | 各 22 组通过 | 各 22 组通过 |
| 静态 Schema | 各 12 项通过 | 各 12 项通过 |
| 凭据专项（17 项） | 各 15 项通过、2 项 Linux 专属跳过 | 各 17 项通过 |
| 模型协议 / 流式 / CLI / MCP | 各 15 + 18 + 11 + 13 项通过 | 各 15 + 18 + 11 + 13 项通过 |
| 检查点续跑 / cron | 各 18 + 12 项通过 | 各 18 + 12 项通过 |
| 后台调度 / 监督 | 各 18 + 16 项通过、1 项环境跳过 | 各 19 + 18 项通过 |
| 自启（14 项） | 各 10 项通过、4 项 Linux 专属跳过 | 各 13 项通过、1 项 Windows 专属跳过 |
| 原 Windows LPAC 审批套件（25 项） | 各 25 项通过 | 各 25 项因平台跳过 |
| 客户端契约 / 服务逐帧校验 | 各 16 + 28 项通过 | 各 16 + 28 项通过 |
| Python 集成合计（不含静态 Schema） | 各 215 项通过、7 项跳过，共 222 项 | 各 198 项通过、26 项跳过，共 224 项 |

统计见 [验证回执](../research/credentials/verification.json)，执行与汇总脚本为 [verify.py](../research/credentials/verify.py)、[summarize.py](../research/credentials/summarize.py)。凭据专项日志：[Windows Debug](../research/credentials/windows-debug-credentials-final.log)、[Windows Release](../research/credentials/windows-release-credentials-final.log)、[Linux Debug](../research/credentials/linux-debug-credentials-final.log)、[Linux Release](../research/credentials/linux-release-credentials-final.log)。其他套件 `*-final.log` 位于同目录；重复运行只计一次，历史完整平台回归不冒称本轮重跑。

凭据测试使用随机命名的合成密钥条目和模拟 HTTP/MCP，实际覆盖三种模型协议、缺失时无 HTTP/MCP/数据库副作用、引用不一致拒绝、缺字段拒绝、轮换后续跑不重复工具执行、运行中 worker 取得新值、同一任务保持旧值、错误/流式/数据库脱敏，以及独立审批和审批后续跑的构建输出脱敏。Linux 的两种审批脱敏路径真实使用隔离构建；这不代表原 Windows 专属 25 项审批场景在 Linux 运行过。

Windows 实际调用 Credential Manager 读写、更新和删除测试条目。Linux 用 [私有服务包装器](../research/credentials/linux_secrets.py) 在独立 D-Bus 会话和临时 XDG 目录运行真实 gnome-keyring；实际验证集合锁定时读取、更新、删除和模型执行都拒绝，解锁后原值未改变。测试服务来自单独下载并验签的 gnome-keyring 51.1 / gcr 包，仅解压到测试目录，没有安装系统包、改动 HOME、真实用户密钥环、登录注册或 linger。Agent 的 libsecret 后端不调用解锁接口；[测试专用 helper](../research/credentials/unlock_test_keyring.c) 只用于私有集合解锁和回读。

排查记录：`agent-tests --redaction-only` 实际复现了多密钥前缀重叠导致后缀泄漏，改为在原文上合并重叠区间后，同一命令及四配置原生/流式回归通过。首次凭据套件的[Windows](../research/credentials/windows-release-credentials-fixture-initial.log)和[Linux](../research/credentials/linux-release-credentials-fixture-initial.log)失败来自夹具仍假设缺失凭据会创建数据库，以及 daemon `--unlock` 未恢复被 D-Bus Lock 锁定的集合；分别调整为无数据库断言和私有服务专用解锁回读。包安装尝试因旧镜像索引的 HTTP 404 在安装前终止，随后仅更新隔离包索引并提取签名测试包。

[原审批测试](../research/credentials/windows-release-build_proposals-previous-contract.log)曾假设移除密钥后仍可批准。现有安全契约要求批准前取得源凭据以脱敏新回执，测试改为先核对缺失时未创建决定或执行，再恢复凭据验证无模型请求的单次执行；纯拒绝另行验证不依赖密钥。最终 Windows Debug/Release 的 25 项套件均通过。

未进行真实供应商认证、在线/本地推理、用户注销/登录或重启；实际登录启动进程能否连接其凭据库仍需在对应用户环境验证。Windows 独立后台启动成功路径仍受外层 Job 限制，拒绝且队列未变的分支已检查。stdin 期限/取消和服务提示竞争未作为本轮独立运行场景计入覆盖范围。

Linux Release 产物名称保持不变，更新的依赖说明包含 libsecret/GLib。镜像源码/测试/文档一致性、可执行文件与归档字节/权限/SHA256、交付程序初始化和凭据能力声明见 [交付回执](../research/credentials/delivery.json)。两端配置恢复为 Release；测试凭据条目、进程、自启登记、私有密钥环、隔离资源、工作区数据库和文档链接检查见 [最终静态检查](../research/credentials/final-static.json)。

## 前次监督诊断验证（2026-10-24）

新增持久监督运行记录、独立生命周期事件流，以及 CLI `scheduler-supervisor-status` / `scheduler-supervisor-events` 和对应 RPC。`scheduler-status` 同时返回 `supervisor` 诊断对象。状态库升级为 schema 7，不读取或迁移 schema 6 等旧库；协议和检查点仍为 2，机器契约现覆盖 35 个方法。Windows、Linux 各自的 Debug / Release 均完成配置、全部目标编译及以下相关回归：

| 检查 | Windows Debug / Release | Linux Debug / Release |
|---|---|---|
| C++ 原生行为测试 | 各 21 组通过 | 各 21 组通过 |
| 自启集成测试（共 14 项） | 各 10 项通过、4 项 Linux 专属跳过 | 各 13 项通过、1 项 Windows 专属跳过 |
| worker 监督及诊断 | 各 16 项通过 | 各 18 项通过 |
| 后台调度（共 19 项） | 各 18 项通过、1 项环境跳过 | 各 19 项通过 |
| cron / 检查点续跑 | 各 12 + 18 项通过 | 各 12 + 18 项通过 |
| 客户端契约 / 服务逐帧 Schema 校验 | 各 16 + 28 项通过 | 各 16 + 28 项通过 |
| Python 用例合计 | 各 118 项通过、5 项跳过 | 各 124 项通过、1 项跳过 |

[11 项静态契约检查](../research/supervisor-diagnostics/schema-final.log) 全部通过。完整统计见 [验证回执](../research/supervisor-diagnostics/verification.json)，可由 [回归脚本](../research/supervisor-diagnostics/verify_subset.py) 和 [日志汇总脚本](../research/supervisor-diagnostics/summarize.py) 复现。监督诊断日志：[Windows Debug](../research/supervisor-diagnostics/windows-debug-supervisor-final.log)、[Windows Release](../research/supervisor-diagnostics/windows-release-supervisor-final.log)、[Linux Debug](../research/supervisor-diagnostics/linux-debug-supervisor-final.log)、[Linux Release](../research/supervisor-diagnostics/linux-release-supervisor-final.log)；其他套件的 `*-final.log` 位于同一目录。前次完整平台回归只作为历史证据，不计作本轮重跑。

真实父子进程测试覆盖异常退出、退避、次数耗尽、停止栅栏、监督器被结束后的子进程回收，以及新运行将未完成旧记录标记为 `superseded`。查询保留原历史阶段，独立返回当前出生身份观测；不会因查询而伪造已退出状态或退出码。原生测试验证 Windows 创建时间身份、Linux 启动标识和进程开始时间、身份不匹配、缺失进程与非法身份。`matching_process` 只说明查询时身份相符，不证明健康、未来存活或授予进程控制权限。

存储验证涵盖状态与事件同事务提交、失败回滚、旧运行/owner 拒绝、有限重启计数、退出码 nullable 语义、时钟回拨及分页边界。心跳更新不追加事件；事件以独立 `seq` 排序，可按游标继续读取。worker 与 supervisor 在同一 SQLite 快照中按 owner/generation 关联，`worker_matches` 不声称 worker 仍在运行。真实 CLI/stdio 测试覆盖无状态查询不创建数据库、非法参数无执行副作用、终态记录、分页重连和逐帧契约校验；关闭进程后的记录对比避免心跳竞争。

Windows 独立后台启动成功路径仍受本机外层 Job 限制；测试验证拒绝且队列未改变，调度套件对应一项环境跳过。监督/契约套件中的拒绝分支通过不表示后台脱离成功。Linux 后台监督、客户端退出后重新连接查询/停止已实际运行。测试使用临时工作区、临时自启登记和模拟 HTTP/MCP；未触发真实登录、注销、重启或 systemd 用户管理器 reload/start，未修改 linger，也未执行在线或本地模型推理。

Linux Release 交付包含更新的可执行文件、协议和客户端示例，保留原产物名称；镜像源码/测试/文档一致性、归档权限、SHA256 与交付程序初始化能力见 [交付回执](../research/supervisor-diagnostics/delivery.json)。两端配置恢复为 Release；进程、临时隔离资源、自启登记、工作区数据库、Python 语法与文档链接检查见 [最终静态检查](../research/supervisor-diagnostics/final-static.json)。

## 前次监督登录自启验证（2026-10-24）

Windows Task Scheduler 与 Linux systemd 用户单元新增显式监督模式，CLI `scheduler-autostart-enable --supervise` 和 RPC `scheduler/autostart/enable` 使用相同参数与校验；普通模式仍为默认。协议保持 2、数据库保持 schema 6、检查点保持 version 2。本轮相关回归的 `*-final.log` 统计如下，每个平台的 Debug、Release 均完成构建：

| 检查 | Windows Debug / Release | Linux Debug / Release |
|---|---|---|
| C++ 原生行为测试 | 各 20 组通过 | 各 20 组通过 |
| 自启集成测试（共 14 项） | 各 10 项通过、4 项 Linux 专属跳过 | 各 13 项通过、1 项 Windows 专属跳过 |
| worker 监督集成测试 | 各 13 项通过 | 各 15 项通过 |
| 后台调度集成测试（共 19 项） | 各 18 项通过、1 项环境跳过 | 各 19 项通过 |
| 客户端契约集成测试 | 各 15 项通过 | 各 15 项通过 |
| 服务场景及逐帧 Schema 校验 | 各 27 项通过 | 各 27 项通过 |
| Python 用例合计（重复复测不重复计数） | 各 83 项通过、5 项跳过 | 各 89 项通过、1 项跳过 |

[10 项静态契约测试](../research/supervised-autostart/schema-final.log) 全部通过。相关回归日志：[Windows Debug 原生](../research/supervised-autostart/windows-debug-native-final.log)、[自启](../research/supervised-autostart/windows-debug-autostart-final.log)、[监督](../research/supervised-autostart/windows-debug-supervisor-final.log)、[调度](../research/supervised-autostart/windows-debug-scheduler-final.log)、[契约](../research/supervised-autostart/windows-debug-contract-final.log)、[服务](../research/supervised-autostart/windows-debug-service-final.log)；[Windows Release 原生](../research/supervised-autostart/windows-release-native-final.log)、[自启](../research/supervised-autostart/windows-release-autostart-final.log)、[监督](../research/supervised-autostart/windows-release-supervisor-final.log)、[调度](../research/supervised-autostart/windows-release-scheduler-final.log)、[契约](../research/supervised-autostart/windows-release-contract-final.log)、[服务](../research/supervised-autostart/windows-release-service-final.log)；[Linux Debug 原生](../research/supervised-autostart/linux-debug-native-final.log)、[自启](../research/supervised-autostart/linux-debug-autostart-final.log)、[监督](../research/supervised-autostart/linux-debug-supervisor-final.log)、[调度](../research/supervised-autostart/linux-debug-scheduler-final.log)、[契约](../research/supervised-autostart/linux-debug-contract-final.log)、[服务](../research/supervised-autostart/linux-debug-service-final.log)；[Linux Release 原生](../research/supervised-autostart/linux-release-native-final.log)、[自启](../research/supervised-autostart/linux-release-autostart-final.log)、[监督](../research/supervised-autostart/linux-release-supervisor-final.log)、[调度](../research/supervised-autostart/linux-release-scheduler-final.log)、[契约](../research/supervised-autostart/linux-release-contract-final.log)、[服务](../research/supervised-autostart/linux-release-service-final.log)。汇总、产物与静态检查分别见 [验证回执](../research/supervised-autostart/verification.json)、[交付回执](../research/supervised-autostart/delivery.json)、[静态检查](../research/supervised-autostart/final-static.json)。

自启测试使用临时工作区，Windows 真实登记 Task Scheduler 任务并通过 COM 回读；Linux 使用临时 `XDG_CONFIG_HOME` 发布单元/链接，并用 `systemd-analyze` 检查两种模式的语法。覆盖默认普通模式、监督参数边界、幂等启用、模式及限制冲突拒绝、先禁用再切换后身份保持、只读查询及不存在项禁用无状态库/进程副作用。状态与删除能识别实际保存的监督限制；篡改或缺失元数据、非规范数字、执行参数不符、额外自动重启策略均拒绝控制。Windows `restart_count: 0` 与 Linux `Restart=no` 均有实际定义回读断言，防止系统通过再次启动监督器重置有限重启次数。

真实 stdio RPC 覆盖 `supervised_autostart` 能力、监督自启登记与状态/删除结果、严格参数拒绝及无 ID 通知无副作用，客户端契约逐帧核验 `supervised` 和条件限制字段。所有临时登记均按已核验身份移除，未为开发工作区保留永久自启项；测试未触发用户注销/登录、系统或 WSL 重启，也未 reload/start 用户管理器或修改 linger。真实平台触发到进程启动的端到端链路仍未实测，登记成功不证明未来登录一定执行。

Windows Debug 的调度套件首次在 EOF 夹具中并发执行审批与 worker 轮询，触发执行锁竞争，见 [首次失败日志](../research/supervised-autostart/windows-debug-scheduler-initial.log)。夹具改为启动 worker 前拒绝提案，再用延迟的模拟模型响应跨过 stdin EOF，直接验证 EOF 不结束 worker；之后 Windows Debug 重跑 19 项调度套件并通过（含 1 项环境跳过）；Windows Release 的完整调度套件此前已通过，修正后另行[补跑 EOF 单项](../research/supervised-autostart/windows-release-scheduler-recheck.log)通过。本轮未为解决测试竞争而放宽产品的审批或执行锁规则。

修正后的 EOF 夹具在 [Linux Debug](../research/supervised-autostart/linux-debug-scheduler-recheck.log) 和 [Linux Release](../research/supervised-autostart/linux-release-scheduler-recheck.log) 均补验通过，计入原有用例而不重复累计。首次 Linux 补验遗漏临时 cgroup 委派，导致前置提案夹具未进入预期审批状态，日志保留在 [环境失败记录](../research/supervised-autostart/linux-debug-scheduler-environment.log)；恢复与正式套件相同的委派后通过，未改动全局 cgroup 设置。

Windows 的调度跳过项来自当前宿主外层 Job 禁止独立后台脱离；该用例先检查明确拒绝且队列未改变，再标记环境跳过。前台监督及父子进程回收在两种配置中均执行；Linux 独立后台监督和客户端退出后控制路径实际运行。模型交互均使用本地模拟 HTTP/MCP，未访问 Ollama、下载模型或执行真实推理。以下旧里程碑仅作历史证据，不将其完整平台套件计为本轮重跑。

## 前次有界 worker 监督验证（2026-10-24）

新增显式 `scheduler-supervise` / `scheduler-start --supervise` 和 RPC 监督选项，保留协议 2、数据库 schema 6。每种平台的 Debug、Release 均通过本轮相关回归：

| 检查 | Windows Debug / Release | Linux Debug / Release |
|---|---|---|
| C++ 原生行为测试 | 各 20 组通过 | 各 20 组通过 |
| 新增监督集成测试 | 各 13 项通过 | 各 15 项通过 |
| 调度、cron、检查点续跑 | 各 48 项通过，1 项环境跳过 | 各 49 项通过 |
| 客户端契约、服务逐帧 Schema 校验 | 各 14 + 26 项通过 | 各 14 + 26 项通过 |
| Python 用例合计（重复复测不重复计数） | 各 101 项通过，1 项跳过 | 各 104 项通过，无跳过 |

[9 项静态契约测试](../research/supervisor/schema-final.log) 通过；[汇总回执](../research/supervisor/verification.json) 对应 [可复现验证脚本](../research/supervisor/verify_subset.py)。最终监督测试日志：[Windows Debug](../research/supervisor/windows-debug-supervisor-recheck.log)、[Windows Release](../research/supervisor/windows-release-supervisor-recheck.log)、[Linux Debug](../research/supervisor/linux-debug-supervisor-recheck.log)、[Linux Release](../research/supervisor/linux-release-supervisor-recheck.log)。其余日志在同目录，`*-final.log` 为相关回归，`*-recheck.log` 为最后重启回执计数修正后的重建及原生、监督、客户端契约、服务复测；未将此前整个平台套件冒称本轮重跑。

测试使用真实父子进程、SQLite 和模拟 HTTP/MCP，覆盖异常退出、总重启次数上限、指数退避期间停止与互斥、过期 epoch/generation 拒绝、监督进程死亡回收、已完成写入不重放及未知进程效果阻断。Linux 额外检查监督进程 SIGTERM 取消与 worker 正常退出不重启。客户端用真实 stdio 验证参数拒绝、能力声明和监督启动回执；Linux 还验证原客户端关闭后新客户端仍可停止监督进程。新锁路径的工具写入拒绝已验证。

首轮 Windows 的锁保护用例已观察到正确拒绝，但测试随后读取仍被独占的文件，导致 PermissionError；已调整到停止后检查字节，原失败日志保留为 `windows-release-supervisor-initial.log`。实现审阅补齐了 Windows 创建时原子 Job 绑定、generation 上限检查，以及仅在实际启动替代进程后累加重启计数。

Windows 受本机外层 Job 限制，后台脱离成功路径仍无法在此环境验证；新增测试明确核对拒绝分支无任务执行或数据库改动，原调度套件有一项对应环境跳过。前台监督及父子 Job 生命周期在两种构建中均实测通过。Linux 后台监督成功路径已实测。所有测试都使用临时工作区，未登记真实用户自启或使用在线/本地推理。

监督器自身不自动重启；该里程碑验证时登录自启仅运行普通 worker，本轮已增加上文的显式监督自启。`scheduler/status` 是最后 worker 记录，不证明监督器存活。新增受保护锁路径参与精确权限绑定；升级前的检查点若与当前保护策略不一致，会按已有规则拒绝续跑，不改写授权或历史格式。交付与清理见 [产物回执](../research/supervisor/delivery.json) 和 [静态检查](../research/supervisor/final-static.json)。

## 前次客户端接入契约验证（2026-10-24）

本轮修改集中于 stdio 服务与客户端契约，协议仍为 2，数据库和检查点格式不变。机器可读定义覆盖 33 个方法、4 类通知；静态校验核对 Schema、引用、方法清单、参数边界与重试分类，实际管道测试校验响应、通知和关联完成结果。Windows/Linux 的 Debug、Release 均构建成功，每种配置通过以下检查：

| 检查 | Windows Debug / Release | Linux Debug / Release |
|---|---|---|
| C++ 原生行为测试 | 各 20 组通过 | 各 20 组通过 |
| 新增客户端契约集成测试 | 各 13 项通过 | 各 13 项通过 |
| 原有服务场景及逐帧 Schema 校验 | 各 26 项通过 | 各 26 项通过 |
| 示例客户端只读查询与离线任务 | 两种配置均通过 | 两种配置均通过 |

[7 项静态契约测试](../research/client-contract/schema-final.log) 全部通过。本轮没有环境跳过。详细运行日志：[Windows Debug 原生](../research/client-contract/windows-debug-native-final.log)、[契约](../research/client-contract/windows-debug-contract-final.log)、[服务](../research/client-contract/windows-debug-service-final.log)；[Windows Release 原生](../research/client-contract/windows-release-native-final.log)、[契约](../research/client-contract/windows-release-contract-final.log)、[服务](../research/client-contract/windows-release-service-final.log)；[Linux Debug](../research/client-contract/linux-debug-final.log)、[Linux Release](../research/client-contract/linux-release-final.log)。示例证据：[Windows](../research/client-contract/windows-clients-final.log)、[Linux](../research/client-contract/linux-clients-final.log)。

新增覆盖包括：会话身份、旧 session 拒绝、运行中只读查询、64 次完成回执淘汰、UTF-8 大结果显式省略、错误在截断前脱敏、取消竞态、显式续跑、重启后持久副作用保留，以及事件订阅错误。真实提案的审阅、拒绝并继续、重复决定跳过均使用实际 HTTP/SQLite/管道，确认只有一次决定与反馈、未执行被拒构建。所有模型调用使用模拟服务。

本轮重跑与接口变更相关的测试；下面的完整平台套件为前次 Linux 移植基线，不将其所有项目计为本轮重跑。Schema 不替代宿主权限与业务校验；操作缓存不是持久幂等机制，缓存缺失和错误不能授权重放。CodeStudio Next 自身的界面和实际接入仍由其项目验证。产物与静态检查见 [本轮交付回执](../research/client-contract/delivery.json)、[静态检查](../research/client-contract/final-static.json) 和 [Windows 静态检查](../research/client-contract/final-static-windows.json)。

## 前次 Linux 移植验证

| 检查 | Debug | Release |
|---|---|---|
| Linux 配置与全部目标编译 | 通过 | 通过 |
| Linux 20 组 C++ 行为测试 | 通过 | 通过 |
| Linux 167 项 Python 集成测试 | 全部通过，无跳过 | 全部通过，无跳过 |
| 实际 C++26 隔离编译/汇编/链接/运行 | 通过，计入 8 项构建集成测试 | 通过，计入 8 项构建集成测试 |
| UID 65534 非特权补充验证 | 20 组 C++ 与真实 C++26 构建通过 | 20 组 C++ 与真实 C++26 构建通过 |
| Windows 20 组 C++ 行为测试 | 通过 | 通过 |
| Windows 203 项 Python 集成测试 | 199 通过、4 跳过 | 199 通过、4 跳过 |
| Windows 2 项真实 XMake C++26 隔离构建 | 2/2 | 2/2 |

Linux 入口为 [verify-linux.sh](../scripts/verify-linux.sh)，WSL 路径和委派要求见 [linux.md](linux.md)。完整日志：[Linux Debug](../research/linux/verify-linux-debug-final.log)、[Linux Release](../research/linux/verify-linux-release-final.log)、[Windows Debug](../research/linux/windows-debug-regression.log)、[Windows Release](../research/linux/windows-release-regression.log)。Windows 的 4 项跳过为 3 项 Linux systemd 专属测试，以及受宿主嵌套 Job 限制的 detached worker 成功路径；该路径先断言拒绝、队列未改变和无模型请求，随后标记环境跳过。Linux 的独立后台启动成功路径实际运行并通过。

Linux 测试环境为 Arch WSL2、内核 `6.18.33.2-microsoft-standard-WSL2`、GCC `16.2.1`、XMake `3.1.0+20260808`、curl `8.21.0`、SQLite `3.53.4`、OpenSSL `3.6.3`、nlohmann-json `3.12.0`、Bubblewrap `0.11.2`。构建依赖采用系统 pkg-config 包。Debug 和 Release 均额外以现有 UID 65534 运行全部 20 组原生测试及真实 C++26 构建，结果位于各自完整日志末尾；未创建账户、修改 WSL 默认用户或更改全局控制器。

本轮 Linux 沙箱验证实际 kernel namespace、seccomp、no_new_privileges、宿主文件与网络拒绝、只读工具树、继承 fd、私有环境及 fork/unshare 拒绝；验证 cgroup 的 pids/memory/cpu 限额、取消、超时、stdout/stdin EOF、正常退出，以及真实宿主 SIGKILL 后后代消失。构建测试核对三代后代、原工作区不变、失败不导出、独立产物目录和逐文件 SHA256、批准只执行一次、回执只消费一次。C++26 夹具显式导入 cc1plus/as/ld，用 pack indexing 编译为 ELF，再在沙箱内运行；未把编译命令加进核心或产品默认清单。

已复现并修复：GCC 16 C++26 的 ranges 字符串字面量约束、systemd WorkingDirectory 与 ExecStart 不同的转义规则、Arch 动态库目录别名、关闭标准描述符时管理 fd 被覆盖。关闭 stdio 的红基线见 [closed-stdio-red.log](../research/linux/closed-stdio-red.log)，修复后的正常与非特权完整原生测试均覆盖该边界。构建步骤生成 FIFO/symlink 作为下一步 executable 的红基线见 [fifo-executable-red.log](../research/linux/fifo-executable-red.log)；修复后使用 anchored/no-follow/nonblocking 普通文件快照解析 ELF，带 1 秒上限的原生回归确认宿主不会被阻塞。测试内临时 cgroup 按唯一身份回收；真实宿主崩溃后的文件残留与子进程存活分开检查。

各平台 Python 用例统计如下；Windows 两次真实 XMake 构建另计：

| 套件 | Linux | Windows |
|---|---:|---:|
| Provider HTTP | 15 | 15 |
| CLI 与 SQLite | 11 | 11 |
| 流式与三种模型协议 | 18 | 18 |
| 本地模型模拟服务回归 | 15 | 15 |
| MCP | 13 | 13 |
| 检查点续跑 | 18 | 18 |
| OS 沙箱 | 5 | 5 |
| stdio 任务服务 | 26 | 26 |
| 工作区隔离构建、审批及真实编译 | 8 | 19 + 25 |
| 后台调度 | 19 | 19（1 环境跳过） |
| Cron | 12 | 12 |
| 自启 | 7 | 7（3 Linux 专属跳过） |
| 合计 | 167 | 203（199 通过） |

前一 Windows cron 里程碑及其时间夹具修正日志保留在 research/build-isolation/verify-cron-*.log；它们不代替本轮 Linux 和 Windows 回归证据。

Release 交付目录为 `build/linux/x86_64/release/`，包含 `codestudio-agent`、`codestudio-agent-linux-x86_64.tar.gz`、运行说明、构建机动态库清单和 `SHA256SUMS`。已核对源码、测试和构建脚本与最终测试镜像一致；镜像中的锁文件差异仅为 XMake 生成的 Linux pkg-config 记录，Windows 依赖记录未改动。二进制从 Linux 镜像复制后的 SHA256 与压缩包内字节一致，在工作区交付路径运行 `doctor` 成功。详见 [交付回执](../research/linux/delivery.json)。这是 Arch x86-64 动态链接产物，未宣称其他发行版兼容。

最终静态检查见 [final-static.log](../research/linux/final-static.log)：Python 源码通过 AST 解析，文档相对链接目标存在；未发现项目残留进程、临时隔离 cgroup/目录、真实用户配置中的 CodeStudio Agent 自启项或工作区数据库。Windows XMake 仍为 Windows x64 / clang-cl / Release，Debug 与 Release 产物均保留。检查未启动后台 worker 或登记自启。

## 已完成的前次工作区清理

按用户授权删除了 `.demo-workspace/.codestudio/state.sqlite3` 和 `build/windows/x64/release/.demo-workspace/.codestudio/state.sqlite3` 两份旧状态库，以及配套 WAL、SHM、运行锁和空状态目录。删除前核对路径位于本工作区、没有重解析点、文件可独占打开，且没有活动项目进程。已移除的构建插件库、对象文件、依赖缓存和旧 Python 字节码缓存也已清理。

全工作区按 SQLite 文件头扫描，确认没有剩余数据库；文件名检查也没有遗留数据库旁文件或旧插件构建产物，结果见 [清理与静态检查日志](../research/build-isolation/verify-cleanup-static.log)。Debug/Release 对清理后的演示工作区执行 `history` 均明确报告数据库不存在，未重新创建状态目录。临时新工作区的 demo 则正常创建当前格式并完成任务。

当前源码与配置中的 `v1` 字样仅用于供应商现行 API 路径；诊断 token 已去掉旧版本后缀。对不支持版本的拒绝检查继续保留，以防旧数据进入运行流程。

## 状态、恢复与权限覆盖

C++ 与 CLI 测试验证了当前状态库的读写、事务可见性，以及不支持数据库版本的整体拒绝。拒绝路径核对原数据库字节、journal 设置和版本保持不变。仅空的未版本化数据库可初始化；合法用户表 `sqliteXdata` 也会被识别为非空，不受 SQL 通配符影响。检查点封装、内核状态和绑定均须满足 version 2；错误类型、浮点版本和其他版本均被拒绝，并验证失败不推进账本或检查点。

续跑测试覆盖持久化批次游标、工具回执复用、累计轮次预算、调用 ID 唯一性、配置与工具 schema 精确绑定、凭据脱敏及当前凭据替换。SQLite 故障注入检查任务、事件、动作与检查点的原子性；真实宿主终止用例检查 `recover` 对已派发动作标记 `unknown`，并阻止可能重放未知副作用的续跑。测试还确认终态任务和事务失败不会错误推进会话状态。

文件工具与导入/导出测试覆盖路径上溯、别名、重解析点、硬链接、保护路径、复制数量与字节预算，以及取消时的清理边界。需要权限的动作必须先获得授权，未授权进程或 MCP 服务器不能启动。相关契约见 [检查点续跑](resume.md)、[恢复契约](resume-contract.md) 与 [沙箱](sandbox.md)。

## 模型协议与 MCP 覆盖

HTTP 集成测试使用本地模拟服务，覆盖 Chat Completions、Responses 和 Messages 的流式与非流式响应。检查内容包括提前交付文本、UTF-8 字节分片、多行 SSE、chunked 编码、终态、截断、错误、取消、usage、端点拼接和工具结果关联。未完整接收的模型响应不能触发工具动作；原生 continuation 和完整授权绑定参与恢复验证。

MCP 测试覆盖显式授权、启动前整份清单校验、握手、分页、工具和 schema 预算、冻结工具记录、参数校验、真实 stdio 往返、stderr 分离和凭据过滤。本地配置只接受整数 version 2；C++ 和 CLI 回归验证其他版本及非法类型会在插件启动、模型请求前拒绝，缺失版本也不会隐式补齐。启动标记用于检查失败发生在插件启动之前；插件超时、崩溃或非法回执的效果不能被假定为成功或安全可重试。MCP 与构建工具共享扩展预算，发现阶段也必须核对实际返回的 schema 大小。

本地模型功能按用户要求暂停后续开发。本轮仅运行现有模拟服务回归，检查列表、回环地址、认证、超时、重定向拒绝和合成工具交换，未访问 Ollama、下载模型或进行真实推理。

## Windows 隔离与产物覆盖

单进程工具模式 `--sandbox appcontainer` 与工作区构建模式分别验证。前者限制自包含 executable 和单个活动进程；后者允许私有工作区中的构建进程树，始终使用 Windows LPAC。测试核对实际令牌、AppContainer SID、capabilities、完整性级别及 Job 配置，不能只以启动参数或网络超时作为隔离证据。

测试使用未隔离对照与真实操作检查宿主文件访问、网络访问、工具副本写入、私有 scratch 读写、后代创建和 breakaway 边界。单进程模式与构建模式的 capability 集不同，网络拒绝的具体阶段可能不同。构建进程树须继承资源限额；超时、取消、stdin EOF 和宿主崩溃须回收对应子进程及临时盘符映射。清理测试注入 junction，并检查外部 sentinel 不受影响。

工作区构建测试确认原源码、清单和宿主已有输出不变，每次成功生成独立产物包。宿主校验导出文件的路径、字节数和 SHA256，并写入 schema 2 的 `manifest.json`；测试核对包内清单与返回回执一致。失败、缺失输出、取消或超时不能发布部分包。有效续跑复用已有动作回执和产物，不重复执行或导出；变更清单、后端或授权必须拒绝。

[真实 XMake 夹具](../tests/fixtures/xmake-build/) 与 [集成脚本](../tests/xmake_build_integration.py) 执行 `explicit-v2`、`custom-cache-v2` 两个变体，实际复制本机已安装的 XMake、Clang 和 lld-link，在 LPAC 内完成配置、编译/链接、运行三个显式步骤，再导出 PE 文件并核对哈希。夹具使用无 SDK/STL 依赖的最小工程，通过 C++26 pack indexing 和 `static_assert` 检查语言特性；缓存目录由夹具清单显式声明。

通用执行契约见 [工作区隔离构建](workspace-builds.md#通用步骤与状态格式)。

## 构建提案与任务服务覆盖

提案测试覆盖三种模型协议的 `propose_build` 工具、默认不开放授权、完整 version 2 提案回执和摘要校验。提出方案及只读 review 不能导入源码、启动构建或发布产物；只读 review 不能因查询而创建数据库。计划、工作区、保护路径或凭据约束发生变化时，审批必须拒绝。

审批测试检查唯一决定、执行任务、prepared 动作与审计事件的原子提交；重复批准复用原执行任务，拒绝不能转为批准。故障注入、真实宿主终止及取消覆盖决定不重放、派发效果未知时不自动重试，以及保护配置文件不被导入构建副本。批准任务与源会话独立，源 Agent 续跑必须再次提供相同提案授权。成功提案停在原 tool call 的 `waiting_approval` 检查点（退出码 3）；三种协议均验证批准和拒绝的原生结果关联、HTTP 503 后恢复时请求内容一致且只消费一次。同轮多提案按游标逐个审批；不同轮的新提案也不能复用旧审批。

`build.proposal.consumed` 事件、标准工具结果和游标推进在同一事务提交。C++ 故障注入验证事件写入失败时完整回滚，重开后不重复回注；已记录构建结果但未结束执行任务的崩溃窗口经 recover 后可读取明确证据，unknown/partial 一律阻断。已知失败构建作为明确证据交给模型，可继续分析或提出新方案。

CLI `--continue-task` 与 stdio `continue_options` 验证原权限、精确提案关联和原累计预算；重复旧决定不推进后续提案，取消和 stdin EOF 不继续模型。续跑校验失败保留已提交的审批及独立构建结果。新增测试还确认：审批后的提案内容被改动而摘要不变时拒绝回注；新增执行反馈与当前密钥冲突时，在 MCP 启动及模型请求前拒绝，两者均保持源任务、检查点和事件不变。两个针对性用例在修复前二进制上均复现失败，修复后通过。详见 [构建提案与审批](build-proposals.md)。

stdio 服务测试使用真实父子进程管道，覆盖 protocol 2 初始化、只读查询、共享账本、任务和事件分页、schema 2 事件订阅及重放、流式文本与脱敏、活动期间查询和取消、互斥锁、busy 拒绝、授权一致性、恢复、严格参数校验和无 ID 通知处理。还检查 stdin EOF、shutdown、stdout 断开、输出背压和超长输入的退出及进程回收行为。详见 [任务服务协议](task-service.md)。

## 后台调度覆盖

原生测试验证 schema 6 队列与只读查询、checkpoint revision、原绑定与预算、重复登记、领取 token 和 generation 隔离、退避与期限、手动接管和取消。SQLite 故障注入覆盖任务/作业启动、结束及检查点的原子回滚。同一 job 跨两次提案分别等待明确决定，空轮询不消耗尝试次数。恢复只处理已关联调度任务，保留已提交回执；已派发无回执的动作转为 unknown 并阻止自动继续。

19 项调度集成测试使用真实 CLI、SQLite、HTTP 模拟服务和子进程，覆盖三种模型协议的原生历史续跑、延迟、取消、退避、预算、期限、审批、双 worker 互斥、配置变化及当前环境凭据。真实终止 worker 后检查安全恢复和工具不重放；无调度关联的前台中断任务保持不变。独立 `scheduler-run` 在 stdin EOF 后仍继续处理任务，停止活动任务会保留其他队列，空闲停止保留延迟与审批等待项。自定义状态库旁的 `.scheduler.run.lock` 无法被模型文件工具覆写。

stdio 测试另外验证队列在客户端 EOF 后保留、前台活动期间仍可取消登记、非法参数及无 ID 通知没有调度副作用。后台启动器的拒绝路径验证详见上文；只读探针确认脱离内层 Job 后仍留在不允许继续脱离的 `KILL_ON_JOB_CLOSE` 外层 Job，未使用系统 broker 或任务计划绕过。用法和生命周期见 [后台续跑](scheduler.md)。

## Cron 与登录自启覆盖

新增三组原生测试覆盖五字段语法、列表/范围/步长、星期日别名、日期与星期 OR、月年边界、闰年世纪例外、固定 IANA 时区和夏令时缺失/重复分钟，包含非整小时偏移转换。Store 测试覆盖模板校验、只读查询、唯一触发身份、任务/检查点/job/计划推进的原子提交、漏跑合并、防重叠、暂停/恢复/取消、次数上限、未知效果暂停及所有权隔离。

修复前的 [重叠回归日志](../research/build-isolation/cron-overlap-red.log) 明确复现了长任务结束后错误补跑其运行期间触发槽的问题。修复将终态与下一触发时间推进放入同一事务，并覆盖人工接管、审批等待、时钟回拨及空闲停机后正常补跑。注入计划更新失败时，任务、job、checkpoint revision 和事件一并回滚。修复后的原生验证见 [针对性日志](../research/build-isolation/cron-native-fixed.log) 和本轮 Debug/Release 日志。

12 项 cron 集成测试使用真实 CLI、SQLite、worker 和 HTTP/MCP 夹具，验证三种模型协议各次任务和预算独立、登记不请求模型、去重、次数耗尽、遗漏合并、审批逐次确认、运行中取消、配置改变阻断、崩溃恢复不重放工具、未知效果暂停和当前环境密钥。集成测试移动临时数据库的触发时间以避免分钟等待；精确时间推进由原生测试验证。服务新增 4 项覆盖异步登记、EOF 后已提交计划保留、活动操作期间控制、参数/通知无副作用，以及 EOF 取消正在进行的 MCP 发现并阻止登记。

自启原生测试检查 SID/工作区/数据库身份、Unicode 与空格参数、普通权限、登录触发，以及改变 action/principal/trigger/settings 后拒绝匹配。4 项真实 Windows 集成测试在临时工作区登记、COM 回读、幂等启用、拒绝轮询参数冲突并移除任务；只读查询与不存在项禁用均不创建数据库或运行 worker。首次测试发现 Task Scheduler 将 URI 改写为任务路径，已据原生保存格式修正并通过复测；该次残留临时注册按精确身份核对后清除，见 [清理记录](../research/build-isolation/autostart-cleanup.log)。未为开发工作区保留自启项。

未触发用户注销、登录或系统重启；真实登录事件到进程启动这一端到端路径仍未实测。`Hidden` 只隐藏计划任务登记，不保证控制台无窗口；当时的自启验证仅覆盖普通 worker，没有配置崩溃自动拉起。此次验证没有把 Task Scheduler 用作绕过父 Job 限制的即时启动 broker。

此前 Windows cron 里程碑的静态检查见 [历史检查](../research/build-isolation/verify-cron-static.log)。Linux 自启另有 7 项临时 XDG_CONFIG_HOME 验证，包括精确单元/链接归属、冲突拒绝和 systemd-analyze 语法检查；不 reload、不 start、不配置 linger。未模拟真正注销/登录或 WSL 重启后自动触发。

## 验证边界

- 本轮覆盖 Windows x64 和 Arch WSL2 x86-64，不代表其他 Linux 发行版、架构或 macOS 已经验证。HTTP fixture 不能证明任意云端服务或本地模型兼容；当前协议范围为文本和 function tools。
- MCP 覆盖 stdio、Streamable HTTP、合成服务上的 OAuth，以及锁定插件包加载和隔离；不包含真实服务商登录、插件市场或完整 JSON Schema 支持。
- LPAC 与 Windows ACL 不是虚拟机，也不构成完整系统资源白名单。Job 配置核对不等于持续负载性能测试；运行期间没有 OS 强制磁盘空间配额。宿主崩溃用例核对进程和映射生命周期，不证明崩溃后所有暂存文件均已清理。
- 真实 XMake 夹具不下载工具或依赖，不能证明任意复杂工程、SDK 或第三方包都能在隔离环境构建。
- 续跑可由显式 `resume`、审批继续选项或持久登记后的 worker 触发；cron 使用固定模板创建独立任务。尚无未知结果人工核销、文件系统回滚或加密状态存储。提案摘要锁定配置，不能证明源码和编译器仍是历史字节。
- 当前 stdio 服务只支持单个活动前台操作，后台 worker 独立消费已有任务队列；尚无 HTTP 服务。桌面 UI 归属独立项目 CodeStudio Next，其接入未计入本文验证。管道断开或输出超限时最终通知可能无法送达，应查询持久化账本。
- 同一数据库同时只支持一个固定工作区的 worker；Windows 支持当前用户登录自启，不支持未登录系统服务。Windows/Linux 可显式启用有界 worker 监督及监督自启，监督器自身不自动拉起。运行中期限采用协作取消，不是 OS 硬截止。Linux 后台启动、cron 与 systemd 用户单元登记已验证；用户管理器由会话与既有 linger 策略决定，WSL 发行版本身必须先启动。Windows 无受限父 Job 环境的隐藏启动成功路径仍待实测。
- `completed` 表示模型循环正常结束，不代表独立验收通过。本文不将先前实验或历史日志计作本轮验证结果。
