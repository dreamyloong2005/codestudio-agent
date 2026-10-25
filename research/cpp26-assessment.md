# Agent / Harness 的语言与插件运行时：C++26 专项评估

研究日期：2026-10-21（Asia/Shanghai）。本文件使用本轮实际获取的一手资料；编译器网站是持续更新的页面，不是已发布编译器二进制的实测报告。下文将“官方页面事实”与“设计建议”分开。未安装或编译验证 GCC、Clang、MSVC，未做性能基准，因此不把支持表等同于完整生产可用性。

## 1. 结论

**C++26 可以成为新的 Agent 宿主的技术方向，但不宜把所有 C++26 大特性都设为第一版的不可替代依赖。** 如果团队擅长 C++，项目需要原生桌面、资源控制、现有 C/C++ 库或本地推理集成，采用 C++ 宿主完全合理。稳妥的交付方式是：固定工具链，使用经验证的 C++23 功能子集作为跨平台发布基线，把 C++26 反射、contracts、`std::execution` 以及 modules 的采用分别门控。这里的“C++23 基线”也不是承诺每家编译器已完整支持 C++23；MSVC 官方仍记录 `/std:c++23preview` 的预览和 ABI 提醒，必须按具体编译器与标准库组合验证。[S2][S3][S4][S6]

如果团队没有明确语言偏好，我会优先选择 **Rust 作为长期运行、持有权限的本地核心，TypeScript 作为 UI 和插件 SDK，Python 作为可选的数据、评测和模型工具进程**；服务端优先且团队熟悉 Go 时，可直接以 Go 替代 Rust。这是对开发成本、原生资源管理、内存安全和生态的工程判断，不是跑分结论，也不是必须同时引入三种语言。第一版只应保留核心语言加 UI/SDK 语言两条主要构建链。

对 Agent 而言，任务能否恢复、权限是否真实隔离、失败后是否会重复提交外部动作、上下文是否正确、成本是否受控，通常比宿主采用 C++23 还是 C++26 更直接影响产品质量。CPU、RSS、启动时间、模型延迟和工具等待占比应先测量，再决定是否用 C++ 替换热点组件。

## 2. 本轮核对到的 C++26 状态

| 主题 | 本轮获取的官方页面事实 | 对方案的约束 |
|---|---|---|
| 标准状态 | isocpp 的 Current Status 页面仍写 “C++26 work in progress”；GCC 页面写 C++26 预计 2026 年发布，同时称支持为 experimental。两个持续更新页面的表述并不完全同步。 | 不能仅依据年份宣称 C++26 已正式发布、已经全链路稳定。本报告不作该声明。[S1][S2] |
| GCC 语言支持 | GCC 表列 reflection `P2996R13` 为 GCC 16，要求 `-freflection`；contracts `P2900R14` 为 GCC 16；页面对 C++26 实验支持及兼容性有明确说明。 | 可以选择固定 GCC 工具链开展真实 C++26 开发，但必须把构建参数、补丁版本、标准库和验证结果记录在发布矩阵中；不可直接外推至 Windows/macOS 工具链。[S2] |
| Clang 语言支持 | 本轮获取的 Clang 表对 `P2996R13` reflection、`P2900R14` contracts 均标 `No`。 | Clang 实验分支或第三方 fork 的实现不等于主线支持；跨编译器发行不能无条件依赖这两项。[S3] |
| MSVC | Microsoft 主一致性表未列出上述 reflection / contracts 的支持结果；页面引导到独立 STL C++26 tracker；`/std:c++latest` 文档说明可能含未完整实现的工作草案和实验特性。 | 只能说“本轮资料未确认”，不能将主表缺项直接写成“MSVC 完全不支持 C++26”。`/std:c++latest` 不是完整实现保证。[S4][S6] |
| 标准库实现 | libc++ 的 C++26 表列出 `P2300R10 std::execution`、`P2996R13 Reflection`、`P2900R14 Contracts`，本轮这些条目的状态/首个发布版本列未标 `Complete`。 | 编译器接受 `-std=c++26` 不证明这些标准库能力可用；需要分别探测编译器语言能力和标准库能力。[S5] |
| modules | modules 是 C++20 的语言机制，不是 C++26 才出现的插件能力。CMake 文档仍列生成器、编译器及 `import std` 的组合限制，明确 header units 不支持，并指出 BMI 兼容范围很窄。 | 内部模块可渐进采用；不要把 BMI、C++ 类布局或 `import std` 当成跨编译器第三方插件 ABI。[S2][S3][S7] |

特别注意：本轮 MSVC 主一致性表对 `P2465R3` 的标准库模块项写 `no`，而 CMake 新版文档讨论了可用 MSVC 工具链上的 `import std` 支持组合。不同页面有粒度和更新时间差异。应使用锁定工具链的真实构建验证解决，不以单张表作跨版本断言。[S4][S7]

## 3. C++26 的价值与边界

### 3.1 Reflection：减少胶水代码，而非实现动态插件隔离

`P2996R13` 设计的是以编译期反射为核心的机制，包含反射运算符、splicer 和元函数；提案明确它是将来继续扩展的核心，而不是反射与元编程的终点。它可以帮助生成字段遍历、序列化、工具 schema、配置描述和日志格式。[S8]

建议用它产生 **显式允许对外的 DTO** 的序列化器和 schema，并检验生成结果；不自动暴露宿主内所有类型、私有成员或敏感字段。编译期能枚举成员，不代表该成员适合成为公开工具参数。公共协议仍必须有稳定字段名、版本和兼容规则。

可以做一个只在受控 GCC 工具链上运行的 schema 生成器，输出普通 JSON Schema / C++ / TypeScript 工件，使发布端不依赖反射运行时；但该方案增加了一条生成链，只有真正减少维护工作时才值得使用。反射本身不提供热加载、沙箱、权限控制或跨编译器 ABI。

### 3.2 Contracts：内部不变量，不是授权机制

当前公开工作草案明确四种求值语义：`ignore`、`observe`、`enforce`、`quick-enforce`；`ignore` 不求值谓词，`observe` 可在处理后继续，终止语义则可能终止进程。[S9]

因此 contracts 适合开发和受控运行中的内部前置条件、后置条件和不变量，例如状态机转换、计数边界和对象生命周期约定。对文件路径、网络地址、secret、预算、插件权限等安全边界，必须使用始终执行、可审计、返回明确错误的普通运行时检查；不能依赖可能关闭的契约，也不能让恶意插件输入轻易触发整个宿主退出。

### 3.3 `std::execution`：异步组合很有价值，取消仍需系统设计

`P2300R10` 提供 sender / receiver、scheduler 与异步组合模型，成功、错误和停止是不同完成通道。提案明确：支持取消在 sender 和 receiver 两侧都是可选的；若 sender 不查询 stop token，可不响应 stop request；取消存在竞态，操作可能已经完成。[S10]

对 Harness 的实用设计是统一 `TaskScope`，让 deadline、cancel、预算和 trace 随父子任务传播，区分“已请求取消”“子任务已结束”“外部操作结果未知”。C++26 并不会自动实现跨平台子进程树终止、HTTP 中止、远端副作用回滚或数据库一致性。

第一版可在内部薄接口后使用成熟异步库，例如 Asio 类事件循环配合 C++20 协程；以后仅在实现完整且验证通过的工具链上切换 sender / receiver 实现。不同时引入多套 competing async runtime。任何方案都需给非协作的工具进程设置宽限期和最终 OS 级终止，并对外部已提交操作采用幂等键或查证流程；“停止等待”不能伪装成“外部操作已撤销”。gRPC 官方取消指南同样要求应用代码参与停止工作，协议取消不自动终止 handler 内任意计算。[S10][S11]

### 3.4 内存安全、FFI 和 ABI

C++26 的这些新增能力不构成整个 C++ 语言的内存安全保证。C++ 宿主仍要通过所有权规则、RAII、边界检查、审查、Sanitizer、fuzzing 和隔离控制悬垂引用、越界及并发错误。Rust 官方所有权教程解释编译器如何通过所有权防止双重释放等错误；这也是选择 Rust 处理解析外部输入和权限核心的一个实际理由，但 Rust 中的 unsafe、FFI、逻辑授权错误和依赖漏洞依然需要控制。[S12]

原生 ABI 必须讲清条件。MSVC 有跨多个 Visual Studio 版本的二进制兼容承诺，但带链接器、Redistributable、`/GL`、`/LTCG` 等限制；这不等于不同厂商编译器、任意 STL、任意构建选项之间都有统一稳定 C++ ABI。[S13] 对可信内部动态库，使用版本化 C ABI、opaque handle、明确内存释放方，不让异常、STL 容器和编译器私有类型跨边界；对第三方生态，优先走进程协议。

## 4. 语言选择矩阵（工程判断）

| 方案 | 我会让它负责什么 | 主要优势 | 需要承担的成本 |
|---|---|---|---|
| Rust | 本地 daemon、调度和持久化、进程/权限 broker、文件与网络边界 | 原生资源管理；safe Rust 的所有权约束适合长期进程；与 C 接口集成可控 | 学习和异步所有权复杂度；unsafe/FFI 仍需隔离；不能消除授权与恢复逻辑错误 |
| Go | 云端 worker、远程 runner、调度服务，也可承担本地 daemon | goroutine、标准网络库、`context` 适合 deadline/cancellation 传播；运维部署简洁 | GC 和 FFI 的成本需测量；`context` 是协作取消；官方 FAQ 提醒 C 互操作会放弃相应的内存安全和栈管理保证 [S14][S15] |
| TypeScript | UI、IDE 扩展、插件 SDK、Provider 适配；需要快速交付时也能承载整个控制面 | 前后端共享协议、LLM/Web 生态接入快、开发者上手容易 | 外部数据仍需运行时 schema 校验；类型注解会被擦除；JS 插件不能仅靠 `node:vm` 当安全沙箱 [S16][S17] |
| Python | 评测、数据处理、模型/检索工具与研究原型；也可运行服务端 Agent | 数据和模型工作流适配方便，实验周期短 | 打包与环境管理要标准化；CPU 密集工作与异步循环应隔离；`asyncio` 取消仍需清理逻辑和正确传播 `CancelledError` [S18] |
| C++23 / C++26 | 原生桌面宿主、本地推理/索引、需要精细资源控制的核心或 worker | 对现有 C/C++ 生态、系统 API、本地计算集成直接；C++26 可减少 schema 胶水并改善异步表达 | 工具链/标准库矩阵、生命周期与并发审查、编译成本和第三方二进制边界；新增特性需逐项验证 [S2–S10][S13] |

不要把模型 API 请求的延迟、网络时间、子进程执行时间都算成语言运行时开销。选择 C++ 的正确依据是团队经验、部署形态、可测资源指标或原生依赖，而不是“Agent 更聪明”。同样，选择 Rust 也不意味着不需要 OS 沙箱、权限系统和恢复协议。

## 5. 插件系统：协议优先，隔离方式分层

### 5.1 默认：独立进程 + JSON-RPC

对本地插件优先采用 JSON-RPC 2.0 over stdio；远程高吞吐、多语言服务可另提供 gRPC/Protobuf，必要时用本地 socket / named pipe。传输协议与权限模型分离，协议本身不提供 OS 沙箱。JSON-RPC 定义请求、通知和响应格式；取消、流式事件、生命周期、权限、重试及幂等语义需要宿主协议显式补充。[S19][S11]

独立进程可统一支持 C++、Rust、Go、Node.js 和 Python 插件，绕开宿主语言和 C++ ABI 绑定，便于崩溃恢复、版本升级、监控及结束失控插件。代价是启动与 IPC 成本；通过懒启动、复用、批处理、输出限制和大内容的 artifact reference 处理。只有明确性能需求才引入共享内存，并单独设计所有权协议。

**独立进程只提供故障边界，不自动形成权限隔离。** 未沙箱化的同用户进程通常仍可自行访问文件、网络、环境和凭据。实现上应把它分成两个等级：可信进程插件可按显式信任运行；不可信插件必须进入平台可用的受限账户、容器、OS 沙箱或远端隔离 runner，并通过 broker 使用获准资源。某个平台无法落实权限时，显示真实能力范围或拒绝该插件要求，不能伪称已经沙箱化。

### 5.2 可选：WASM Component Model / WASI

适合纯数据变换、策略计算、上下文过滤、格式化和部分轻量工具。WIT 可描述接口、world、类型、资源和版本化 package；Wasmtime 官方安全文档说明 WebAssembly 的实例内存隔离和 WASI 文件访问的 capability 模型。[S20][S21]

设计时仅导入必要 host functions，限制内存、执行时间/指令预算及输出，文件和网络仍由宿主代理。WASM 不替代宿主函数的参数校验，也不能自动覆盖任意现有 Python/Node 工具、GUI 自动化、复杂本地 SDK 或所有 OS API。支持面应以实际 SDK、runtime 和目标平台验证，先不把它设为唯一插件形态。

### 5.3 可信特例：原生 DLL / `.so` / `.dylib`

仅用于官方、经过审计且确有性能/宿主集成需求的组件。它们与主进程共享地址空间，不应作为不可信第三方扩展的默认执行方式。用版本化 C ABI 控制兼容边界；更高风险的原生组件也可放进独立插件进程。[S13]

### 5.4 TypeScript 插件的易用性与边界

提供优质 TypeScript SDK 很值得，但实现宜运行在独立 Node.js worker **进程**，配合平台沙箱及 broker。不要将 `node:vm`、同进程 `import()` 或普通 worker thread 作为恶意插件的安全隔离承诺：Node.js 官方明确写 `node:vm` 不是安全机制，不用于执行不可信代码。[S17]

### 5.5 稳定扩展面与权限原则

推荐第一版只公开以下扩展面：`ModelProvider`、`ToolProvider`、`ContextProvider`、`Renderer`、`Evaluator`。记忆后端和调度策略可在需求成熟后开放；审批决策、审计日志真实性和权限授予始终由宿主控制。插件 hook 的前后顺序、可写字段、失败策略和时间预算必须固定，避免任意 hook 改写全部内部状态。

每个插件 manifest 至少包含：插件 ID/版本、协议范围、入口和 runtime、工具 schema、所需能力、资源预算、包摘要。调用时绑定会话、workspace、身份和最小能力范围；权限是运行时判定，不是“安装时点过同意”就永久拥有所有上下文。

密钥由宿主保管，尽可能通过能力代理使用；若插件必需接触原始密钥，则把这一事实单独列为敏感权限，并严格控制作用域。源码签名与摘要能确认供应链身份和完整性，不能证明插件没有恶意行为。审计记录插件版本、输入摘要、授权范围、输出/产物引用、用时、费用及副作用状态，默认做 secret 脱敏。

调用状态建议为 `requested → authorized → running → succeeded | failed | cancelled | unknown`。`cancelled` 只用于确认工作已结束且语义清楚的情况；网络断开、远端提交后超时等使用 `unknown` 并查证。重试只对明确幂等的调用自动进行；创建工单、提交 PR、部署和付款类操作需要幂等键、查询或补偿流程。协议层约定不能自行提供跨外部系统 exactly-once 保证。

## 6. 一条能落地的 C++ 路线

1. **固定发布矩阵。** 先选操作系统/架构，再锁定编译器、标准库、SDK、CMake/生成器和依赖版本。初版核心使用验证过的 C++23 子集；如 MSVC 某功能尚不稳定，该功能用成熟替代实现。不要将 `CMAKE_CXX_STANDARD 26` 当能力检测。
2. **建立窄而稳定的内部接口。** `TaskScope` 管理取消/超时/预算；`EventStore` 管理事件与状态投影；`ToolBroker` 管理工具和授权；`ProviderAdapter` 处理模型供应商差异；`PluginHost` 管理进程与协议。网络/async 具体库藏在内部，公共插件协议不暴露模板和 STL 类型。
3. **功能探测 + 行为测试双门控。** 对 reflection 使用相应 feature-test macro 与 `try_compile` 的最小源码验证；对 `std::execution` 验证实际使用的 sender、scheduler 和标准库接口；对 contracts 验证发布配置中采用的语义；对 modules 额外验证增量构建、安装和消费。宏存在只表示一个线索，不证明应用需要的完整能力。
4. **C++26 先用于可替换组件。** 反射生成工具 DTO/schema；contracts 检查内部不变量；sender / receiver 放在 async backend；modules 先限于内部独立模块。跨平台核心语义和公开协议不依赖实验分支的私有行为。
5. **用故障场景检验系统。** 主进程重启、插件崩溃、流式断连、超时与取消竞态、重复投递、schema 升级、磁盘满、并发文件修改和外部副作用未知，比证明一段 reflection 示例能编译更能验证 Harness。

如果目标是单一受控平台、团队接受锁定 GCC 工具链，并且 C++26 能显著减少核心实现复杂度，可以直接用 C++26 语言模式开发；同时保持进程插件协议稳定，并记录特性/工具链风险。若目标是开放插件生态加 Windows/macOS/Linux 同期发行，C++23 验证基线加 C++26 门控更合适。这是两种产品约束下的选择，不是禁止采用新标准。

## 来源与本轮抓取依据

- **[S1]** Standard C++ Foundation, Current Status：https://isocpp.org/std/status 。本轮正文含 “C++26 work in progress”。
- **[S2]** GCC, C++ Standards Support：https://gcc.gnu.org/projects/cxx-status.html 。本轮读取 C++26 支持、reflection、contracts、C++20 modules 条目及实验性提示。
- **[S3]** Clang, C++ Support：https://clang.llvm.org/cxx_status.html 。本轮读取 `P2996R13`、`P2900R14` 为 `No` 的语言表项。
- **[S4]** Microsoft, C/C++ language conformance：https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance?view=msvc-170 。本轮页面标最后更新 2026-10-16；读取主表及其 C++26 STL tracker 链接。
- **[S5]** libc++, C++26 Status：https://libcxx.llvm.org/Status/Cxx26.html 。本轮 `P2300R10`、`P2996R13`、`P2900R14` 行有跟踪 issue，未标 `Complete`。
- **[S6]** Microsoft, `/std` language version：https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version?view=msvc-170 。读取 `/std:c++23preview` ABI 提醒及 `/std:c++latest` 的实验/未完整实现说明。
- **[S7]** CMake, `cmake-cxxmodules(7)`：https://cmake.org/cmake/help/latest/manual/cmake-cxxmodules.7.html 。读取 compiler/generator 支持、`import std`、header units 和 BMI 兼容性限制。
- **[S8]** WG21, `P2996R13 Reflection for C++26`，2025-06-20：https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2996r13.html 。读取引言、编译期相关限制及示例。该提案是设计依据，后续修订可能改变细节。
- **[S9]** C++ 公开工作草案，`[basic.contract.eval]`：https://eel.is/c++draft/basic.contract.eval 。本轮读取四种求值语义及各自行为；这是工作草案的公开 HTML 镜像，不是已发布 ISO 标准。`P2900R14` 原始 PDF 也实际获取并确认地址为 https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2900r14.pdf ，本轮未提取 PDF 正文，因此具体语义以已读取的草案为依据。
- **[S10]** WG21, `P2300R10 std::execution`，2024-06-28：https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r10.html 。读取异步组合介绍及 4.9.2–4.9.3 的可选取消与竞态；提案为设计来源，不将其旧接口名称视作最终接口清单。当前公开工作草案交叉检查：https://eel.is/c++draft/exec 。
- **[S11]** gRPC, Cancellation：https://grpc.io/docs/guides/cancellation/ 。读取取消传播以及服务端处理取消的责任。
- **[S12]** The Rust Programming Language, Ownership：https://doc.rust-lang.org/book/ch04-01-what-is-ownership.html 。读取所有权与 double-free 防范说明。
- **[S13]** Microsoft, C++ binary compatibility：https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017?view=msvc-170 。读取跨版本 ABI 承诺和链接器、Redistributable、`/GL`、`/LTCG` 限制。
- **[S14]** Go, FAQ：https://go.dev/doc/faq 。读取 C 互操作对内存安全和栈管理保证的限制。
- **[S15]** Go, `context` package：https://pkg.go.dev/context 。读取取消传播与 cancel 函数的职责。
- **[S16]** TypeScript, The Basics：https://www.typescriptlang.org/docs/handbook/2/basic-types.html 。读取 “Erased Types” 中 “Type annotations never change the runtime behavior of your program”。
- **[S17]** Node.js, `node:vm`：https://nodejs.org/api/vm.html 。明确写 “The node:vm module is not a security mechanism. Do not use it to run untrusted code.”
- **[S18]** Python, asyncio tasks：https://docs.python.org/3/library/asyncio-task.html 。读取 Task cancellation、Task groups 和清理要求。
- **[S19]** JSON-RPC 2.0 Specification：https://www.jsonrpc.org/specification 。本轮实际读取概述、transport agnostic 描述、Request/Notification/Response 结构；它是消息协议规范，不定义宿主 OS 权限模型。
- **[S20]** Bytecode Alliance, WIT：https://component-model.bytecodealliance.org/design/wit.html ；Why Component Model：https://component-model.bytecodealliance.org/design/why-component-model.html 。读取类型、接口、world 和 package 描述及跨语言内存表示问题。
- **[S21]** Wasmtime, Security：https://docs.wasmtime.dev/security.html 。读取 WebAssembly 隔离与 WASI filesystem capability 模型；没有将其安全目标理解为“无漏洞保证”。

研究边界：没有独立确认 ISO 正式出版目录的 C++26 发布状态；没有本地编译器实测、产品性能测试或插件沙箱穿透测试；所有架构及语言取舍均为基于来源事实的设计建议。
