# 子进程沙箱

`run`、`resume`、`mcp-tools` 支持 `--sandbox none|appcontainer|linux-namespace`。默认 `none` 保持现有可信工具的运行方式；`appcontainer` 选择 Windows 的低权限 AppContainer（LPAC）后端；`linux-namespace` 选择 Linux Bubblewrap/seccomp/cgroup v2 后端。隔离范围是普通命令与 MCP 子进程，宿主模型连接和文件工具分别遵循原有配置与授权。Linux 要求、打包限制和使用方式见 [Linux 与 WSL](linux.md)，下文主要描述 Windows 后端的普通单程序模式。锁定插件的多文件与显式资源策略见[完整插件包隔离](plugin-sandbox.md)。

本后端的实现与动态验证应分开判断：`doctor` 报告当前构建包含的后端，`sandbox_runtime_probe: "not-run"` 表示它没有执行隔离探测。实际构建、文件/网络拒绝和失败场景结果以 [验证记录](validation.md) 为准。本页本身不证明某一台机器已经通过这些检查。

## 使用方式

运行适合通过参数和标准输出交换数据的独立可执行文件：

```powershell
xmake run codestudio-agent run --workspace D:\YourProject `
    --sandbox appcontainer --allow-process 'D:\Tools\text-helper.exe' `
    --prompt '使用指定工具处理一段文本，通过标准输出返回结果。'
```

模型接口仍使用已有的端点、模型和密钥配置。`--allow-process` 授予启动指定 executable 的权限，`--sandbox` 选择子进程的隔离方式，两者需要同时考虑。

MCP 清单的 `command` 应为自包含的服务器 executable，再添加同一选项：

```powershell
xmake run codestudio-agent mcp-tools --workspace D:\YourProject `
    --mcp-config D:\YourProject\mcp.json --allow-mcp text `
    --sandbox appcontainer
```

`run` 使用 MCP 时同样添加 `--sandbox appcontainer`。该模式应用于本次运行的所有 MCP 服务器和普通进程，不支持逐服务器放宽。`mcp-tools` 会实际启动服务器；返回的 `sandbox` 是本次选择的模式。

## 文件、网络与进程边界

| 对象 | AppContainer 模式的处理 |
|---|---|
| 身份 | 每次启动创建唯一的临时 AppContainer profile/SID，无 capabilities，使用 LPAC；回收后删除该 profile |
| 暂存 executable | 只复制被授权的一个 executable 到私有 `bin`；子进程获得读取/执行权限，不能写入 `bin` 或其根目录 |
| 工作目录 | 新建私有 `scratch`，向该 SID 授予修改权限并设置低完整性标签；子进程可以在其中创建临时文件 |
| 原工作区 | 不导入、不挂载，也不向其 ACL 添加沙箱权限；传入绝对路径不会自动授予访问权 |
| 依赖与脚本 | 不复制旁边的 DLL、脚本、解释器运行库、配置或资源；依赖私有文件的程序可能无法启动 |
| 网络 | 不授予网络 capabilities，不增加回环豁免，不修改系统防火墙设置 |
| 后代进程 | Job 设置 `ActiveProcessLimit=1` 且不允许 breakaway，阻止直接后代运行；首版适用于单进程 stdio 工具，不支持工具链进程树 |
| 标准输入输出 | 仅继承指定的 stdio 句柄（普通命令的 stdin 为 NUL），普通命令输出仍受大小限制，MCP 继续使用 JSON-RPC |
| 生命周期 | 子进程通过 JOB_LIST 在创建时原子加入 Job Object；恢复执行前验证身份、实际 LPAC 访问语义和 Job 限制，超时、取消或关闭连接时回收受监督的进程 |

普通命令每次调用都有新的 scratch；MCP scratch 在一条服务器连接内保留，服务器重启或 `resume` 时重新创建。scratch 内容不会写回工作区，也不进入检查点。Windows 还会为该专用 profile 建立其私有系统目录与注册表区域。正常回收会清理私有暂存目录并删除 profile；宿主硬退出、清理失败、过深或过多文件可能留下残留，这些残留不用于续跑。

LPAC 去除普通 AppContainer 对 `ALL APPLICATION PACKAGES` 的广泛隐式访问，但 Windows 的系统资源与显式授予 LPAC 或该 SID 的资源仍受其各自 ACL 控制。因此，不能把这一模式描述为“只能读取 scratch”。启动成功不代表任意第三方程序或解释器均兼容。

单进程限制约束该 Job 的活跃进程数，不是系统范围的进程创建禁令。外部系统代理代为启动的进程不一定继承 Job；本版未提供此类代理接口，也未认证所有 Windows 代理的访问边界。

## 与宿主权限的关系

`read_file` 和 `write_file` 在宿主 broker 中执行。它们继续限制工作区路径、保护状态文件，并由 `--allow-write` 控制写入；它们不在子进程 AppContainer 内。模型可以通过已授权的宿主工具读取内容，并将内容作为参数或 MCP 输入交给子进程。

模型服务 HTTP 请求同样在宿主中执行。禁用子进程网络不会阻断模型接口，也不承诺输入的文件内容不会发送给所配置的模型服务。

`--sandbox none` 中，进程获得当前用户权限，普通进程工作目录是工作区。文件工具的路径检查不会限制这些进程。Job Object 在两种模式下都负责生命周期；它本身不提供文件或网络隔离。

## 续跑和失败处理

原任务使用 AppContainer 时，`resume` 必须再次显式提供 `--sandbox appcontainer` 和原授权。检查点绑定中的沙箱策略使用 version 2，保存模式、网络与文件访问方案；`none` 模式也显式保存 `{"version":2,"mode":"none"}`。模式变化或策略不匹配会在 MCP 启动前拒绝继续。

当前绑定也包含 `child_processes: "deny"` 和 `process_limit: 1`，表示禁止直接后代运行的单进程 Job 策略。这些字段描述普通命令策略。锁定插件使用独立的 `isolated-process-package` 执行绑定和明确的包级资源授权，不继承或放宽普通命令的单进程权限。

不支持的系统、沙箱准备错误或受限进程启动失败会报错，不会自动转为 `none`。外部工具结果不明时，沿用 `unknown` 和禁止自动重放的语义。续跑只能恢复宿主对话与已记录结果，不能恢复插件内存或 scratch；依赖这些状态的插件应自行报告丢失状态。

准备阶段包含 Windows profile 创建及最多 256 MiB 的同步 executable 复制。普通进程在准备后与恢复执行前重新检查取消/截止时间；MCP 在准备及启动边界检查取消和截止时间；完整包复制检查取消并验证冻结摘要。正在进行的同步系统调用无法立即中断。准备失败也会保守地阻止未知结果重放，尚未细分所有“确定未启动”的失败。

## 当前限制

这一后端不是虚拟机，也不对抗宿主上已经拥有同用户或管理员权限的恶意进程。普通 Windows 工具模式尚未实现 CPU、内存配额或工作区导入/导出；独立构建模式提供这些能力，见 [工作区构建](workspace-builds.md)。OS 磁盘容量配额、按目标授权网络、macOS 隔离及对第三方插件的完整安全认证尚未实现。仍需结合插件来源、兼容性及实际边界测试评估使用范围。

实现依据：[Windows 进程创建属性](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)、[AppContainer 隔离](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation)、[临时 profile API](https://learn.microsoft.com/en-us/windows/win32/api/userenv/nf-userenv-createappcontainerprofile)、[Job 限制](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information)及 [Job 继承边界](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)。LPAC 的 AAP/ARAP 访问语义验证参考 [Chromium AppContainer 测试](https://chromium.googlesource.com/chromium/src/+/main/sandbox/win/src/app_container_test.cc)。

## 独立工作区构建模式

需要编译器后代、源码输入、额外依赖和产物回写时，使用 `build --workspace ... --build-config ...` 或模型任务的 `--build-config`，详见 [工作区隔离构建](workspace-builds.md)。它使用独立的版本化 LPAC 构建策略（registryRead、进程树、CPU/内存限额、固定清单和原子导出），本页的普通单进程/零 capability 模式保持原语义。
