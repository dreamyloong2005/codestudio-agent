# macOS 适配路线

CodeStudio Agent 的 macOS 首个目标是 Apple Silicon（arm64）基础运行版。阶段 1 已完成本机 Debug/Release 验收；阶段 2 已实现 Keychain、后台监督和 LaunchAgent 登记，真实登录触发的验收单独记录。隔离能力按当前选择暂不启用；阶段 4 提供可搬移包、用户级安装与签名公证工具。实现状态、测试通过和托管 CI 运行结果分别记录。

核心继续使用 **C++26 + XMake**，mcpp 等生态成熟后再评估迁移。CodeStudio Agent 负责无界面的 Agent/Harness、CLI 和服务接口；桌面 UI 属于 **CodeStudio Next**。本地模型后续开发继续暂停，macOS 适配不包含模型下载、服务管理或内嵌推理。

## 当前状态

当前新增 macOS 原生构建配置、Darwin 子进程启动、进程出生身份、只读清单快照、平台能力声明与基础套件选择。CLI、stdio 任务服务、模型协议、MCP、审批和恢复继续共用业务实现。最终原生通过范围以 [验证记录](validation.md) 与机器回执为准；普通进程组不构成 OS 沙箱。

| 能力 | 当前实现或后续缺口 | 主要代码与文档入口 |
|---|---|---|
| 原生构建与验证 | 已增加 macosx/arm64 配置、C++26/格式化/时区运行探针和 Debug/Release 回执；本机 Debug/Release 验收通过，打包与发布验收见阶段 4 | [XMake 配置](../xmake.lua)、[工具链要求](../scripts/toolchains.json)、[验证入口](../scripts/verify.py)、[打包入口](../scripts/package.py) |
| 普通进程与身份 | Darwin 使用 posix_spawn、明确继承标准流与进程组；出生身份绑定系统启动 UUID 和微秒级启动时间，使用 kqueue 检查查询中的退出 | [进程执行](../src/platform/process.cpp)、[stdio 子进程](../src/platform/line_process.cpp)、[身份观测](../src/platform/process_observation.cpp) |
| 后台 worker 与监督 | 支持独立会话后台启动、绑定父身份的受管 worker、有限重启和停止代数；复用持久队列与 cron | [后台启动](../src/platform/scheduler_process.cpp)、[监督子进程](../src/platform/scheduler_child.cpp)、[调度语义](scheduler.md) |
| 系统凭据与 OAuth | 环境与非交互 Keychain 凭据可用；OAuth 使用独立记录域及跨进程刷新锁，保留崩溃不可重试标记 | [凭据后端](../src/platform/credentials.cpp)、[OAuth 原生能力](../src/mcp/oauth_native.cpp)、[凭据语义](credentials.md)、[OAuth](mcp-oauth.md) |
| 当前用户登录自启 | 当前用户 LaunchAgent enable/status/disable 已实现；不立即执行、不无限重启，真实登录证据单列 | [自启入口](../src/platform/autostart.cpp)、[后台调度](scheduler.md) |
| OS 隔离与工作区执行 | 只读清单快照及安全导入/原子导出前置层已适配，含大小写、Unicode 和 APFS 卷别名保护；隔离执行仍不支持，无宿主回退 | [沙箱入口](../src/platform/sandbox.cpp)、[Darwin 工作区操作](../src/platform/build_workspace_macos.cpp)、[隔离构建](workspace-builds.md) |
| 资源登记与回收 | 非 Windows/Linux 平台使用不支持存根，缺少本机资源身份和回收实现 | [资源接口实现](../src/platform/sandbox_resources.cpp)、[资源生命周期](sandbox-resources.md) |
| CI 与发布交付 | 已定义 macOS ARM64 自托管原生验证、打包和临时安装作业；提供发布签名公证工具，正式验收状态单列，尚无本轮托管运行结论 | [CI 定义](../.github/workflows/native.yml)、[交付说明](delivery.md) |

## 阶段与依赖

| 顺序 | 里程碑 | 状态 | 完成标志 |
|---|---|---|---|
| 1 | macOS arm64 基础运行与原生验证 | 代码已落地，本机原生验收通过 | 原生 Debug/Release 构建与基础功能测试通过，产生对应源码和产物的机器回执 |
| 2 | Keychain、后台监督与登录自启 | 代码已实现；真实登录验收待完成 | 凭据生命周期、后台续跑、cron 和 LaunchAgent 的真实运行场景通过 |
| 3 | 插件、工作区与资源生命周期隔离 | 文件准备层已实现；原生资源和生命周期保证存在实测缺口，整体验收未完成 | 后端方案确认，隔离拒绝、资源限额、取消和崩溃回收得到实际验证 |
| 4 | 安装、签名公证与正式交付 | 工具已实现；本机安装与正式发布验收分别记录 | 干净 Mac 完成安装、升级、卸载及发布包运行验收 |

原生 CI 从阶段 1 开始接入，持续复用统一验证入口；阶段 4 才完成正式分发验收。隔离方案可提前调查，但阶段 1、2 的通过不能代替阶段 3 的验证。

## 阶段 1：macOS arm64 基础运行与原生验证

该阶段在真实 Apple Silicon Mac 上建立可重复的基础运行与测试路径。

实施内容：

- 确定最低 macOS 版本、编译器、标准库、SDK 和 XMake 的组合，并记录实际验证版本。通过编译和运行探针检查项目所用 C++26 模式、`std::format`、`std::chrono` 时区数据库等能力，不仅检查编译器是否接受语言选项。
- 补齐 arm64 的依赖获取、链接、架构标识和原生构建目录。核验 SQLite、JSON、libcurl、OpenSSL 等依赖及其传递依赖，不沿用 Linux 的 libsecret 系统依赖。
- 适配普通进程执行、参数与环境、管道读写、输出上限、超时、取消、子进程退出和必要的进程身份观测。确认文件描述符继承、信号及进程组行为，身份不能仅依赖 PID。
- 检查文件工具在 macOS 文件系统上的路径、符号链接、硬链接、大小写和原子替换语义，保持既有工作区授权边界。
- 复用 CLI、stdio JSON-RPC、SQLite 状态机、任务事件、审批状态和显式恢复逻辑；补齐平台能力声明与错误返回。
- 验证 Chat Completions、Responses、Messages 的流式接入，以及 stdio/Streamable HTTP MCP 的基础通信。协议测试使用受控夹具，真实供应商服务验收另行记录。
- 扩展统一验证入口并增加 macOS arm64 原生 CI，运行 Debug/Release、协议契约和基础集成测试，保存源码指纹、产物摘要、日志与机器回执。

验收条件：

1. 在记录的 macOS/工具链环境中从源码原生构建成功，CLI 和任务服务可以实际运行。
2. 基础模型协议、MCP、文件/进程工具、取消、任务查询和显式恢复通过对应测试；中断后结果未知的动作仍阻止重放。
3. 缺少的后台、系统凭据或隔离能力明确返回不支持；请求隔离不能静默降为宿主执行。构建审批的状态处理与实际隔离构建执行分别标明支持范围。
4. Debug/Release 的通过、失败和跳过分别记录，CI 实际运行结果可追溯；仅增加工作流文件不算验收通过。

阶段 1 不宣称已具备 Keychain、登录自启、强隔离或正式分发能力。调度与 cron 的通用逻辑可以提前验证，后台平台生命周期留在阶段 2。

## 原生构建与验证命令

本轮环境为 macOS 26.5.2 arm64、macOS SDK 26.5、LLVM 22.1.8、XMake 3.0.9（HEAD `2b184e178`）及 Python 3.13.14。编译器和专用运行库的最低部署目标固定为 macOS 15.0；本轮实际运行仅覆盖 26.5.2，不能据此宣称 15.0 或 Intel/Rosetta 已验收。

需要 LLVM 编译器和与之配套、启用 `std::chrono` 时区数据库的 libc++/libc++abi。系统 Apple libc++ 与官方 macOS LLVM 包附带的 libc++ 均未启用所需时区功能，仅替换 `clang++` 不满足要求。

从 [LLVM 22.1.8 官方发布页](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8) 取得 `LLVM-22.1.8-macOS-ARM64.tar.xz`，核对 SHA256 `f260f4f7c0d430828a81ae8a3826a1d63fc0963ec2459489308cc23b1f7eab4f` 后解压，并将 `CODESTUDIO_LLVM_ROOT` 指向包含 `bin/clang++` 的目录。另需 CMake 3.20+、Ninja 和 Xcode Command Line Tools；CMake/Ninja 不在 PATH 时可向下方构建脚本传 `--cmake` / `--ninja` 绝对路径。

[build-macos-libcxx.py](../scripts/build-macos-libcxx.py) 下载并校验固定 LLVM 22.1.8 源码，在工作区构建 libc++/libc++abi，启用实验性时区实现，并将 Darwin 的时区查找指向系统 `/usr/share/zoneinfo/`。脚本要求该目录已有 `tzdata.zi` 和 `leapseconds`，不回退到伪造时区或关闭 cron；完成后写入 `build/toolchains/libcxx-toolchain.json`。构建脚本与统一验证入口均实际运行格式化、IANA 时区和 DST 缺失/重复时间探针。

```sh
python3 -m venv build/contract-python
build/contract-python/bin/python -m pip install -r tests/requirements.txt
export CODESTUDIO_LLVM_ROOT=/absolute/path/to/llvm
build/contract-python/bin/python scripts/build-macos-libcxx.py --llvm-root "$CODESTUDIO_LLVM_ROOT"
export CODESTUDIO_LIBCXX_ROOT="$PWD/build/toolchains/libcxx-22.1.8-tzdb"
export CODESTUDIO_CONTRACT_PYTHON="$PWD/build/contract-python/bin/python"
bash scripts/verify-macos.sh all
```

两个配置顺序执行；产物位于 `build/macosx/arm64/<mode>/`，回执位于 `build/verification/macosx/`。macOS 使用原生 arm64 的 SQLite 3.50.4、nlohmann JSON 3.12.0、静态 OpenSSL 3.6.5（摘要），HTTP 使用系统 libcurl（本轮运行库 8.7.1，SecureTransport）；不加载 Linux libsecret。开发二进制通过运行库路径加载上述专用 libc++，必须保留对应目录，不能把单个 executable 当作独立发布包。发布包和安装验证尚未开放；验证工具为不支持的套件记录明确的跳过原因，而不是成功结果。

能力查询示例：

```sh
build/macosx/arm64/release/codestudio-agent doctor
build/macosx/arm64/release/codestudio-agent demo --workspace .demo-workspace
```

`doctor` 和服务 `initialize` 对 macOS 返回 `background_scheduler: true`、`mcp_oauth: true`（doctor 中为 `mcp_oauth.available`）、`worker_supervision: true`、`login_autostart: true`、`plugin_sandbox: false`、`sandbox_resources: false`。`doctor.workspace_build.available` 为 false。`build_proposals` 与 `approval_continuation` 支持创建、审阅、拒绝提案及随后续跑；提案回执的 backend 为 unsupported，批准请求在提交决定前因缺少隔离后端而拒绝，不能用来推导隔离构建可执行。Keychain 缺失、锁定或不可用时不会回退到已有环境密钥，隔离请求不会降级为宿主运行。

## 阶段 2：系统集成

在基础运行通过后，补齐与 macOS 当前用户会话有关的功能。

实施内容：

- 增加 Keychain 凭据后端，支持现有设置、删除、状态和引用解析语义。每次任务或续跑重新读取凭据，正在执行的操作固定使用取得的值；缺失、锁定或不可用时不回退到明文文件或其他来源。
- 适配 OAuth 原生凭据记录、刷新互斥及崩溃后的未知效果处理，验证登录、刷新、轮换、取消、脱敏和调度预检。保持配置与授权绑定，不因认证错误隐式重发工具调用。
- 实现独立后台 worker 和受管监督子进程，复用持久队列、有限重启次数、指数退避、停止代数和诊断事件。补齐父子进程退出、客户端关闭及 PID 复用相关验证。
- 实现当前用户登录后的 LaunchAgent 登记、查询和禁用，固定 executable、工作区、状态库和监督选项，不保存账号密码或 API Key。
- 保持登记不立即执行、不覆盖冲突登记、禁用不终止当前 worker 的边界。平台自启不能绕过有界监督，以无限自动重启延长任务权限或预算。
- 将 cron 的时区、DST、遗漏合并、不重叠、审批阻断和恢复行为与真实后台运行联测。

验收条件：

1. 在实际用户会话中完成 Keychain 读写、缺失/锁定、轮换及后台读取测试，测试使用独立凭据条目并清理自身数据。
2. 客户端退出后 worker 的行为符合文档；worker 故障后的监督次数与退避有界，停止操作不作用于身份不符的进程。
3. 完成真实登录触发验收；LaunchAgent 文件生成、静态检查或手动启动不能代替登录后启动测试。
4. 续跑和新一轮 cron 仍遵守原授权、各自预算及未知效果阻断规则，不因平台适配扩大权限。

### 阶段 2 验证与真实登录

统一验证入口已纳入 `credentials`、`mcp_oauth`、`scheduler`、`supervisor`、`autostart` 和真实后台 `cron` 场景；机器回执见 [阶段 2 验证记录](../research/macos-stage2/verification.json)。Keychain 锁定测试使用独立临时库并检查用户默认库/搜索列表保持不变；CLI/OAuth 与后台轮换测试使用随机命名的自有条目，结束后删除。后台监督验证父退出前后的拒绝/收敛、固定 owner 与新 generation、次数/退避、停止与未知效果阻断。普通工具自行创建的进程组不属于 worker 组收敛保证。

**真实 GUI 注销/登录验收仍需单独完成，未通过前不能宣称整个阶段 2 验收完成。** 可续接探针为 [verify-macos-login.py](../scripts/verify-macos-login.py)：

```sh
build/contract-python/bin/python scripts/verify-macos-login.py prepare \
  --executable "$PWD/build/macosx/arm64/release/codestudio-agent" \
  --evidence-dir "$PWD/build/macos-login-acceptance"
# 保存其他工作，手动注销此 macOS 用户并重新登录；不要手动 bootstrap 或启动探针。
build/contract-python/bin/python scripts/verify-macos-login.py verify \
  --evidence-dir "$PWD/build/macos-login-acceptance"
# 如要取消这次验收，可随时移除探针自己的登记并停止自己的 worker：
build/contract-python/bin/python scripts/verify-macos-login.py cleanup \
  --evidence-dir "$PWD/build/macos-login-acceptance"
```

prepare 仅登记独立的普通/监督两种 fixture，记录可执行文件与 plist 摘要和 GUI 会话身份，不立即运行，也不注销用户。verify 要求会话确实变化、launchd 使用原 plist 直接启动对应进程、监督身份匹配；随后核对禁用登记保持 worker 存活，并通过 `scheduler-stop` 停止，确认 launchd 没有重启。相同会话、源码重建后产物变化或不完整回执都不能计为通过。操作者须遵守不手动 bootstrap/启动的验收流程；脚本能核对新会话与 launchd 进程关系，但不能单凭事后状态区分新会话中手动 bootstrap 和自动登录加载。可执行文件及其专用 libc++ 路径必须保留到验收结束。脚本不创建用户、不解锁 Keychain、不重置系统登录配置。

## 阶段 3：隔离能力

先确认可维护的 macOS 隔离方案，再实现平台后端。Windows Job/LPAC 与 Linux namespace/seccomp/cgroup 无法直接移植；普通 POSIX 子进程、工作目录或进程组不构成同等的安全边界。

方案验证必须逐项说明文件访问、网络、子进程、CPU、内存、进程数、宿主崩溃以及资源回收的可实现保证。若所选机制无法提供某项保证，应在能力与授权检查中拒绝相应请求，不通过改名或放宽限制宣称支持。具体机制及最低系统要求由原型和原生验证确定。

实施内容：

- 为普通工具、完整插件包和工作区构建分别定义执行边界、允许的输入、运行库依赖和输出行为。
- 实现只读插件/工具链副本、独立 scratch、工作区安全导入、受限执行和校验后的原子产物导出，继续使用通用显式构建清单。
- 将 CPU、内存、进程数及超时限制绑定到实际执行资源，说明计量口径与无法满足时的拒绝行为。
- 移植资源归属登记、创建意图、租约、原生身份捕获、任务关联、分页查询、预览和精确 ID 回收。
- 在宿主、子进程和资源创建/删除边界注入故障，验证活跃、被替换、身份未捕获或所有权不明的资源被拒绝处理。

验收条件：

1. 真实测试确认越界文件访问、网络访问和未授权后代执行被拒绝，插件和隔离构建均覆盖多文件依赖与取消。
2. 资源限额、进程树收敛和宿主崩溃场景实际通过；能力声明不能替代 OS 层效果验证。
3. 显式回收仅操作已登记且身份匹配的资源，不接管历史未登记资源、不向进程发送终止信号、不重放工具或改写任务/检查点状态。
4. 分别报告本轮新增残留与历史总残留；测试清理不能伪装成产品成功回收。

### 阶段 3 原生方案验证结果

本机 macOS 26.5.2 的 [Seatbelt 原型](../research/macos-stage3/seatbelt/README.md) 与 [资源限额原型](../research/macos-stage3/limits/README.md) 已实际运行。它们记录允许和拒绝的差分结果，不是已交付的生产隔离后端。完整结果与复现源文件见 [阶段 3 记录](../research/macos-stage3/README.md)。

| 保证 | 本机观察 | 当前产品处理 |
|---|---|---|
| 文件与网络 | Seatbelt deny-default 能限制宿主文件、只读包、scratch 与网络；同 UID 的信号、Mach lookup、共享内存、Unix socket 也经对照测试拒绝 | 保留原型，不以单项拒绝推导完整隔离可用 |
| 子进程和进程组 | 严格 profile 可拒绝 fork；允许 fork 后可脱离原组，SETEXEC 也可在不 fork 时改变组/会话；进一步拒绝 posix_spawn 会影响编译器 | 不以普通进程组作为一般插件/构建的执行容器 |
| 内存 | `RLIMIT_AS`/`RLIMIT_RSS` 不能直接实现物理内存字节额度；footprint/jetsam 相关接口被本用户权限拒绝 | 不接受未兑现的 memory_bytes 限额 |
| CPU 与进程数 | CPU rlimit 是累计秒数；私有 CPU 百分比限制可自行清除且 fork 子进程不继承；NPROC 按整个 UID 计数 | 不替代现有整组 cpu_percent/processes 契约 |
| 宿主崩溃与回收 | 可信 guardian 可收敛其持有的单个直接子进程；guardian 自身崩溃后负载仍存活，缺少一般进程树空状态证明 | 继续拒绝隔离执行和资源回收，不凭目录名认领残留 |

已实现可独立验证的 macOS 文件准备层：逐级 `openat/O_NOFOLLOW` 锚定路径，拒绝非普通文件/硬链接及快照变化，按 Unicode 规范化与大小写折叠保护路径；来源根位于受保护目录内时拒绝整个来源，并通过锚定目录身份识别 APFS firmlink 别名和根目录重叠，限制文件/字节/目录深度，保留可执行位并清除新对象的继承 ACL。产物在私有目录中复制并生成 SHA256 清单，使用 `renameatx_np(RENAME_EXCL)` 原子发布；取消或发布前失败不会产生正式 bundle。安全清理不跟随链接、不跨挂载、不修改外部目标权限。该层不启动程序，不能据此批准构建或把 workspace_build 标为可用。

当前 `os_sandbox`、`plugin_sandbox`、`sandbox_resources` 与 `workspace_build.available` 仍为 false；完整插件隔离、受限多进程构建及归属回收尚未达到本阶段验收条件。Seatbelt 的公开入口已被 Apple 标为 deprecated/不再支持，其 SBPL 行为也需绑定实际 OS 测试。当前选择保持原生 macOS，暂不启用未满足契约的隔离能力，不引入 Linux 虚拟机执行环境。虚拟化能力查询仅作为研究记录，不代表客体已配置或完成验收；本轮没有安装虚拟机、创建客体或放宽资源与授权约束。完整阶段 3 仍需满足上述原生隔离验收条件。

## 阶段 4：正式交付

基础 CI 继续运行，本阶段补齐面向用户的发布链路。当前提供 macOS arm64 可搬移包、用户级安装/升级/卸载，以及显式 Developer ID 签名、公证和 DMG ticket 装订工具。开发测试包使用 ad-hoc 签名以运行重定位后的 Mach-O，不属于正式签名或公证发布。

包内带时区可用的 libc++/libc++abi 和对应许可，仅加载相对私有运行库及系统依赖；原构建摘要、重定位摘要、最终签名字节及归档摘要分别记录。包内 `verify-delivery.py --standalone` 可在没有源码和编译器的 Mac 上验证临时安装、实际运行、升级及卸载；其机器回执不会仅凭命令参数宣称环境干净。

复现命令见[交付说明](delivery.md)，本轮原生、包字节、临时安装及正式发布各自的实际状态见[阶段 4 回执](../research/macos-stage4/README.md)。阶段 2 真实登录、阶段 3 完整隔离、独立干净 Mac 和 Apple 公证结果均不可由本机包测试替代。

- 扩展用户级安装、升级、卸载和包清单校验；保留用户数据及被修改的文件，并检查升级后的程序实际可运行。
- 完成应用代码签名、公证及分发包验证，明确签名身份、依赖、最低系统版本和架构；摘要校验不等于发布者身份验证。
- 在独立干净 Mac 上验证首次安装、系统启动限制、实际运行、升级、卸载，以及所声明的后台、自启和隔离功能。
- arm64 为首个目标。Intel 原生包或 Universal 包按需求增加，分别验证全部依赖、架构和运行行为，不能以 Rosetta 运行成功替代 Intel 原生验收。
- 保存发布产物与验证回执的对应关系，保留构建产物名称；签名、公证后以最终分发字节重新记录摘要。

完成标志是拟发布能力均有对应的原生验收证据，安装和签名公证流程实际通过。平台测试包、CI artifact 与正式发布包应明确区分。

## 环境前提与共用边界

原生编译和运行验收需要真实 macOS 主机或 macOS runner。Windows/WSL 可以准备适配代码、协议夹具和文档，但不能完成 Darwin 运行时、Keychain、LaunchAgent、系统隔离或签名公证的原生验收。

阶段 1 开始前应落实 Mac/runner、开发工具链和测试用户环境；阶段 2 需要可执行真实登录测试的会话；阶段 4 需要独立干净机器及可用的签名公证环境。CI 应使用受控测试数据，具有权限的自托管 runner 不执行不可信仓库代码。

平台适配保持现有状态库 schema 7，以及任务协议、检查点和本地配置 format/version 2 的规则，不增加旧格式兼容或数据迁移。通用业务状态机和客户端契约继续共用，平台差异集中在后端和能力声明中。

实施进度更新到[开发路线](roadmap.md)，实际通过范围更新到[验证记录](validation.md)和对应机器回执，交付条件同步到[交付说明](delivery.md)。本文的计划状态不替代测试结论。
