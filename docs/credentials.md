# 模型与远程 MCP 凭据

模型任务通过凭据引用选择认证材料。任务配置、检查点和调度参数保存引用，不保存密钥值。每次新任务或续跑开始时解析一次引用，整个操作固定使用取得的值；后台 worker 的下一个任务也会重新解析。

远程 MCP 使用清单条目中的 `credential`，同样支持环境和系统引用。它与模型的 `--credential` 独立：省略则匿名连接，缺失时失败，不借用模型密钥。多个服务按引用分别解析，所有已授权引用解析完毕才建立连接；`mcp-tools` 也使用该规则。完整配置及传输边界见 [MCP 工具扩展](mcp.md)。

## 选择凭据

`run` 和 `cron-create` 接受 `--credential`，默认 `env:CODESTUDIO_API_KEY`。`resume`、`schedule-resume` 及审批后续跑默认使用原检查点保存的引用；显式指定时必须完全相同。

| 引用 | 来源 | 轮换生效时间 |
|---|---|---|
| `env:VARIABLE` | 当前 Agent 进程的指定环境变量 | 进程实际环境更新后；其他终端的修改不会更新已有进程 |
| `system:identifier` | 当前用户的系统凭据库 | 使用相同引用的下一次新任务或续跑，不需要重启 worker |

环境变量名为 1..128 个 ASCII 字符，首字符是字母或 `_`，其余为字母、数字或 `_`。系统标识符同样最长 128 个字符，首字符是字母或数字，其余可含字母、数字、`.`、`_`、`-`。引用和值都是精确匹配；不要将秘密本身用作标识符。

```powershell
codestudio-agent run --workspace D:\YourProject `
  --credential system:work-model --api responses `
  --base-url https://your-provider.example/v1 --model your-model `
  --prompt '阅读 README.md，说明构建步骤。'
```

凭据选择不改变模型地址、协议、工具权限、预算或恢复规则。一个引用只决定认证材料，不能自动切换供应商或账号配置。离线 demo 与直接隔离构建不使用模型凭据；暂停中的本地模型管理能力未因此扩展。

## 管理系统凭据

管理命令只通过 CLI 提供，不创建工作区或状态库。设置和更新使用相同命令：

```text
codestudio-agent credential-set --name work-model --stdin
codestudio-agent credential-status --credential system:work-model
codestudio-agent credential-delete --name work-model
codestudio-agent credential-status --credential env:CODESTUDIO_API_KEY
```

`credential-set` 必须从重定向 stdin 管道或文件读取；不接受命令行密钥参数，也不接受会回显的交互 stdin。下面的 Python 标准库示例关闭输入回显，并直接通过管道传递，密钥不进入子进程参数或 shell 历史：

```python
import getpass
import subprocess

secret = getpass.getpass("模型 API Key: ")
subprocess.run(
    ["codestudio-agent", "credential-set", "--name", "work-model", "--stdin"],
    input=secret.encode("ascii"),
    check=True,
)
```

密钥值须为 1..2048 字节的可见 ASCII（`0x21`..`0x7e`，不含空格）。输入最多允许一个末尾 LF 或 CRLF，读取至 EOF；超长、空值、控制字符、第二行或非法字符均拒绝。stdin 等待期限为 10 秒；可取消。不要为了重定向而把密钥写入项目文件。

命令返回 `reference`、`backend` 和 `state`，不返回密钥。`credential-status` 检查存储值能否读取及其格式，不请求模型，也不证明供应商已接受该密钥。状态含义如下：

| 状态 | 含义 |
|---|---|
| `available` | 读到合法凭据，或设置操作成功 |
| `missing` | 环境变量为空/不存在，或可用凭据库中没有该条目；删除不存在项也返回此值 |
| `locked` | Linux 集合或条目处于锁定状态 |
| `unavailable` | 后端、会话、默认集合、权限或记录格式等条件不满足，无法安全完成操作 |
| `deleted` | 指定的自有系统条目已删除 |

管理命令在 `available`、`missing`、`deleted` 时退出码为 0；`locked`、`unavailable` 或非法参数为 2。模型任务要求 `available`，其他状态都在模型或 MCP 调用前失败，不回退到另一个引用。接口不列举用户的其他凭据。

## 平台条件

Windows 使用当前用户的 Credential Manager Generic Credential，保存于 CodeStudio Agent 专属目标命名空间，持久性为本机用户凭据。读取、更新和删除会核对目标及归属元数据；冲突条目不覆盖。自启必须使用同一个用户，其他用户的凭据不可由同名引用取得。

Linux 使用 `libsecret-1` 连接当前会话的 Secret Service。构建和运行需要 libsecret 及其 GLib/GIO 依赖；服务提供者和已有默认集合由用户管理。Agent 不创建或解锁集合，不显示解锁提示，也不保存解锁密码；已锁定时返回 `locked`，无服务或无默认集合时返回 `unavailable`。每次 native 操作共享 5 秒取消期限；没有明文文件回退。凭据条目按应用/schema/标识符及标签核对，重复或被修改的条目拒绝使用和修改。

Linux worker 必须能连接正确用户的会话总线，且默认集合已可读取。终端、systemd 用户管理器和 WSL 会话的环境可能不同，终端查询成功不能证明登录自启进程也能访问该服务。Agent 不安装或配置 D-Bus 会话、密钥环服务，也不修改 linger 或唤醒关闭的 WSL。服务是否由会话总线自动激活取决于用户已有的系统配置。其他平台的系统凭据库暂不支持。

## 续跑、后台任务和审批

模型配置的 `credential` 是执行绑定的一部分。现行状态数据库仍为 schema 7，协议和检查点仍为 version 2；缺少该字段的旧模型检查点明确拒绝，不补默认值或转换，需要创建新任务。相同引用下值的轮换不改变绑定；改成另一个引用则拒绝续跑，即使两个引用恰好保存相同值。

系统凭据更新后，正在运行的模型操作仍使用开始时取得的值。后续前台任务、显式续跑和调度尝试读取新值。认证失败不触发即时刷新或隐式重放；后台是否能进行下一次尝试，仍由原调度授权、检查点阶段、期限、次数和预算决定。未知工具效果始终阻断继续。

环境引用从 worker/supervisor 启动时继承的环境中读取。轮换这种来源仍需从更新后的环境重启进程；只改变当前终端或旧服务进程外部的变量不会生效。系统引用可以避免为轮换密钥而重启 worker，但不能绕过凭据库的锁定和会话限制。

批准构建本身不请求模型，但新构建回执需要使用源任务保存的凭据引用进行脱敏，所以批准执行前该引用必须可读取。单独拒绝仍可在密钥不可用时提交。批准并继续在同一个操作中固定使用一次解析的值，不在两个阶段之间切换密钥。恢复时仍核对当前密钥不会改变原检查点、签名内容和反馈；需要脱敏改写的历史不能继续。独立决定及构建已经提交后的续跑失败不撤销既有结果。

源任务含远程 MCP 时，批准构建还会解析其保存清单中已授权服务的凭据引用；不重新读取当前清单、不连接 MCP。即使当前清单被修改或删除，构建输出仍按源任务引用脱敏。修改清单后的继续会话仍须满足原授权绑定检查。

## 服务接口与权限边界

stdio 服务仍为协议 2、43 个方法。模型相关 `options.credential` 对应 CLI 选项，可用于 `tasks/start` 的 run 模式、`tasks/resume`、`schedules/create`、`crons/create` 和审批的 `continue_options`。服务不接受 `api_key`，也不提供设置、删除或读取密钥值的 RPC；系统凭据先通过 CLI 管理。UI 继续由 CodeStudio Next 负责。

实际选中的值和既有 `CODESTUDIO_API_KEY` 均进入本次操作的脱敏上下文，包括流式文本、记录及最终错误；操作结束后不缓存所选凭据。任务数据库、提示词、工具输入输出和配置本身仍是明文。脱敏不保证识别所有未知秘密，也不保证识别密钥的编码、拆分或变形。

系统凭据库保护存储，不是工具权限沙箱。同用户的未隔离进程可能通过操作系统 API 读取该用户凭据；自定义环境变量也不等于自动从所有子进程剔除。应只授予可信 executable/MCP，并按需要启用 OS 隔离。把模型认证材料从任务配置中移出，不会降低已授权宿主工具本来的权限。

平台验证、测试夹具和未实测边界以 [验证记录](validation.md) 为准；本文件说明接口和运行条件，不表示真实登录或真实模型认证已经验证。
