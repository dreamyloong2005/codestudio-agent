# 工作区隔离构建

`build` 在 Windows LPAC 或 Linux namespace/cgroup 沙箱内执行固定构建清单：复制指定源码和工具链、运行整个构建进程树、核验并导出声明的产物。原工作区不是构建进程的工作目录。整个流程通过现有动作账本记录，CLI、模型工具和 stdio 服务使用同一实现。Linux 环境与工具链打包要求见 [Linux 与 WSL](linux.md)。

隔离执行模块按通用 v2 清单执行显式声明的可执行文件、参数和环境，不添加工具专用默认值或设置专用环境白名单。XMake 构建能力由测试夹具验证，其他构建工具尚未完成集成验证。CodeStudio-Agent 自身继续使用 C++26 + XMake 构建。

```powershell
codestudio-agent build --workspace D:\YourProject --build-config D:\YourProject\build.json
```

直接 `build` 不访问模型，也不需要 API key。最终 JSON 的 `build` 包含工具回执；`build.output.artifacts.directory` 是产物目录。`show <task-id>` 可再次读取步骤输出和动作记录。失败返回非零退出码，不发布产物包。

模型任务通过 `run ... --build-config <json>` 获得 `build_workspace` 工具。参数只能是 `{}`，模型不能覆盖命令、输入路径、环境或输出位置。`--allow-write` 与宿主文件工具权限独立；构建清单本身列明并授权唯一的产物导出目录。`--sandbox appcontainer` 仍是原来的单进程工具模式，隔离构建不通过该选项启用，也不会降级成 `none`。

需要模型先拟定清单时，可显式增加 `--allow-build-proposals` 开放 `propose_build`。提案只校验和保存计划，用户通过 `build-review` 查看并按摘要批准或拒绝；批准后创建独立隔离构建任务。已有 `build` / `--build-config` 固定清单流程继续工作，两个授权可以同时提供。完整流程与恢复边界见 [构建提案与审批](build-proposals.md)。

## 固定清单

下面展示通用 v2 清单：将工作区中的 `input.txt` 复制到隔离区，运行声明的工具生成 `result.bin`，再导出到新产物包。`my-tool.exe` 是占位工具，使用时应替换为项目实际程序、参数和输入，并将 `source` 改为已安装工具的绝对目录；所需 DLL、SDK 或其他依赖也必须显式列入清单。

```json
{
  "version": 2,
  "inputs": ["input.txt"],
  "mounts": [
    {"name": "tools", "source": "D:/YourTools", "inputs": ["my-tool.exe"]}
  ],
  "steps": [
    {"executable": "${tools}/my-tool.exe",
     "arguments": ["input.txt", "--output", "result.bin"]}
  ],
  "outputs": ["result.bin"],
  "artifact_directory": "artifacts",
  "limits": {"timeout_ms": 120000, "processes": 16, "memory_mb": 2048, "cpu_percent": 80}
}
```

- `inputs`、`outputs`、`exclude`、`cwd` 使用 `/` 分隔的相对路径；`.` 表示整个目录。禁止通配符、上溯、盘符相对路径、ADS、设备名、重解析点、硬链接和 DOS 短路径别名。选择目录时复制整个子树；`.git`、`.codestudio`、状态库及清单、产物目录受保护。建议显式列出源码，避免复制不需要的私有数据。
- `mounts` 是**只读副本**，不是宿主可写挂载。最多 16 个，`source` 必须是绝对本地路径，`inputs` 必填。已安装 SDK、标准库、头文件、预取的包缓存都可以声明为额外输入。包含硬链接的包缓存应先实体化成普通文件。
- `${workspace}` 指向私有源码副本，`${home}`、`${temp}` 指向私有目录，`${name}` 指向对应工具链副本。变量只替换这些声明的根；不会读取宿主环境变量。最多 16 个固定步骤，每步有完整可执行文件路径和 `arguments` 数组，可选 `cwd`，默认 `.`。不经过隐式 shell。
- v2 的 `environment` 最多 64 项，变量名符合 `[A-Z_][A-Z0-9_]*`，长度最多 64。系统变量、私有 home/temp 相关变量由宿主保留；常见凭据名（如 `*_KEY`、含 `TOKEN`/`SECRET`/`PASSWORD`/`CREDENTIAL`）及 `*_PROXY` 被拒绝。值为显式明文，可使用上述根变量；不会复制宿主 PATH、API key 或代理设置。这些名称检查不能识别藏在普通变量值中的秘密，清单和回执也不提供加密。

清单读取上限 1 MiB，拒绝重复 JSON 字段和过深嵌套。在构造扩展时冻结其原始内容；每次授权和执行都会重读核对。v2 还将规范化后的通用执行计划写入恢复绑定。恢复任务必须再次提供同一 `--build-config`；配置、路径、执行计划或后端版本变化均拒绝续跑。源码可以是用户编辑后的新版本，检查点不锁定源码/编译器文件的历史字节。

## 通用步骤与状态格式

当前只接受 version 2 清单，且必须提供显式 `steps`。清单与内部执行计划均使用 version 2，不做版本转换。`doctor.build_manifest_versions` 返回 `[2]`，不代表已探测本机工具链。

工具所需的环境变量和私有缓存目录应通过 `environment` 显式声明，并随清单一起审阅。宿主不会自动补充工具专用环境，也不会自动探测、安装或下载工具。通用步骤适合执行已明确授权的构建、测试、代码生成或打包程序；具体工具的依赖和沙箱兼容性仍需验证。

构建绑定使用 version 2，包含原清单、执行计划、隔离后端和导出策略；提案回执也使用 version 2，二者都不含 `plugin` 标记。Windows 构建后端标识为 `windows-lpac-build-v2`。状态库只接受 schema 7，检查点封装、绑定和内核状态只接受 version 2。遇到其他状态版本时，需使用新状态库或新工作区创建任务；不会转换旧数据库或检查点。

| 模块 | 职责 |
|---|---|
| `normalize_build_manifest` | 接受 v2 清单并生成同版本通用执行计划，不转换版本或添加工具默认值 |
| `WorkspaceBuild` | 对接 `build_workspace`，冻结清单、重查授权、绑定执行计划并调用宿主执行模块 |
| `BuildProposal` / 宿主审批入口 | 前者校验并保存 `propose_build` 回执；后者核对摘要、持久化唯一决定并创建独立执行任务 |
| `WorkspaceExecution` | 验证完整计划，复制输入、执行沙箱进程树、限制资源、处理取消并校验发布产物 |
| ToolBroker / 任务内核 / Store | 权限检查、派发前持久化、动作回执及恢复控制 |

清单解析是普通宿主模块，计划仍需经过 `WorkspaceExecution` 校验。MCP stdio 工具扩展独立工作，没有外部 MCP 调用宿主隔离执行的接口。

## 资源和产物

| `limits` 字段 | 默认 | 允许范围 |
|---|---:|---:|
| `timeout_ms` | 120000 | 100–3600000 |
| `processes` | 64 | 1–256 |
| `memory_mb` | 4096 | 64–32768 |
| `cpu_percent` | 80 | 1–100 |
| `log_bytes` | 65536 | 1024–262144 |
| `source_bytes` | 512 MiB | 1–16 GiB |
| `source_files` | 50000 | 1–200000 |
| `mount_bytes` | 8 GiB | 1–32 GiB |
| `mount_files` | 100000 | 1–200000 |
| `artifact_bytes` | 512 MiB | 1–8 GiB |
| `artifact_files` | 10000 | 1–100000 |

墙钟期限覆盖导入、全部步骤及导出。Job Object 强制约束当前步骤的进程树数量、总提交内存和 CPU 比例，并禁止 breakaway。任何步骤失败、取消或超时都停止后续步骤。步骤返回前必须确认 Job 中全部进程退出；输出按整个构建累计截断，并标记 `output_truncated`。

复制额外受单文件 2 GiB、遍历深度 64 和条目数预算约束。导入/导出字节上限不是运行期间的磁盘配额：当前没有 OS 强制磁盘空间限额。构建会消耗临时磁盘，配额卷/容器或 VM 后端属于后续工作。

所有步骤成功后，宿主复制指定产物到新包，再进行同卷原子重命名：

```text
artifacts/<随机包ID>/
  files/result.bin
  manifest.json
```

每条产物记录包含原相对路径、包内路径、字节数和宿主计算的 SHA256。产物清单使用 `schema_version: 2`，也受大小预算约束。包目录永不复用，不覆盖原源码或旧产物；导出时拒绝链接、不安全路径、缺失输出和预算超限。包内文件不由宿主自动执行；如需测试，应声明为隔离区内的后续步骤。

导出前失败的构建动作记录 `effect: none`。发布成功记录 `effect: applied`。若宿主在派发后崩溃且未写入回执，`recover` 会保留未知结果并阻止自动重放；可能已经发布的包需要人工核对。固定清单任务的安全检查点可通过 `resume` 复用已有回执，不重复构建/导出。显式登记的 [后台续跑](scheduler.md) 使用同一检查点和回执规则，不能重放不明效果或追加构建权限。批准提案创建的独立任务没有模型检查点，不支持 `resume` 或调度登记，再次尝试需要新提案，见 [构建提案与审批](build-proposals.md)。

## 隔离边界

构建使用每次唯一的 AppContainer profile/SID，低完整性、LPAC，唯一的 `registryRead` capability 用于 Windows 子进程启动所需的系统兼容性查询；没有网络 capabilities。宿主核验 token、LPAC AccessCheck 语义以及 Job 实际限制后才恢复首进程。后代继承相同受限身份和 Job。原单进程沙箱继续使用零 capabilities。

每次构建临时占用一个当前未使用的盘符，映射目标仅为私有暂存目录，向工具提供普通 DOS 路径。映射在当前登录会话可见，从 Z 向 D 选择，原子创建且绝不替换已有盘符；没有可用盘符就明确失败。使用非永久、非继承的 Windows 对象句柄，关闭或宿主崩溃后映射自动消失。此路径投影不授予任何额外宿主目录权限，也不按构建工具名称切换行为。

源码/工具链的原 ACL 不变。工具链副本只读，源码副本、home/temp 和本次 Windows profile 可写。取消、EOF、异常或正常结束都会结束本次私有会话；清理遇到不安全或无法删除的内容时保留残留，不扩大权限或跟随链接。残留目录不是恢复检查点。

LPAC 基于 Windows ACL，不是虚拟机或精确的文件可见性白名单；系统明确授予 LPAC/capability 的资源仍可访问。恶意同用户宿主进程并发修改、系统级漏洞、任意外部 broker 的完整隔离证明不在此后端保证范围内。原宿主文件工具与模型 HTTP 仍在各自权限下执行。macOS 后端尚未实现，请求明确失败。

Linux 后端标识为 `linux-namespace-build-v2`，使用 Bubblewrap 私有根、只读工具树、私有工作区，以及 seccomp/cgroup v2 的网络与资源限制。`process_limit` 包含监督进程，至少为 3；步骤结束前回收该会话全部后代才允许导出。输入文件权限收敛为 0600 或 0700（保留属主执行能力），不复制 setuid 或原 ACL。需要委派的 cpu/memory/pids 控制器，配置不足会拒绝执行。工具的头文件、辅助程序、静态库等须显式声明，系统 ELF 运行库按元数据复制；不会隐式挂载宿主工具链。详见 [Linux 与 WSL](linux.md)。

## 验证

```powershell
python tests/build_integration.py build/windows/x64/release/codestudio-agent.exe
python tests/xmake_build_integration.py build/windows/x64/release/codestudio-agent.exe
```

第一套测试使用独立 Win32 fixture，检查多代子进程身份、文件/网络拒绝、产物校验、失败/取消/EOF 清理、模型工具、恢复绑定及不支持版本的明确拒绝。第二套使用 [XMake 测试夹具](../tests/fixtures/xmake-build/)，验证 v2 显式环境配置及自定义缓存目录，实际启动已安装的 XMake、Clang 和 lld-link，在隔离区完成 C++26 编译、链接、运行新产物及导出。XMake 命令和私有缓存配置仅保存在回归夹具中。实际通过情况以 `validation.md` 为准。
