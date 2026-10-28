# 构建、验证与用户级安装

CodeStudioAgent 的运行程序使用 C++26 + XMake。Python 仅用于开发验证、打包和安装管理；已安装的 Agent 本身不需要 Python。包保留原可执行文件名称，用户工作区、SQLite 数据库和系统凭据独立于应用安装目录。

## 构建要求与统一入口

[toolchains.json](../scripts/toolchains.json) 是机器可读的最低要求：Python 3.11、XMake 3.0.5、Windows x64 clang-cl 22 + Visual Studio 2022 C++ 工具/Windows SDK，Linux x86-64 GCC 16，或 macOS arm64 LLVM 22.1.8 及配套时区可用的 libc++。Windows 使用 XMake 锁定的依赖；Linux 使用明确的 pkg-config 系统依赖，包括 libsecret。最低版本不等于任意组合均经过验证；报告记录实际版本及源文件摘要。当前具体环境见 [验证记录](validation.md)。

```powershell
python -m venv build/contract-python
build/contract-python/Scripts/python.exe -m pip install -r tests/requirements.txt
build/contract-python/Scripts/python.exe scripts/verify.py --mode all
```

Linux 将解释器路径替换为 `build/contract-python/bin/python`，须预先提供 `CODESTUDIO_CGROUP_ROOT`，启用 cpu/memory/pids 委派并安装 Bubblewrap；配置步骤见 [Linux 文档](linux.md)。验证入口不安装系统包、不创建用户、不启用全局控制器。`scripts/verify.ps1`、`scripts/verify-linux.sh` 与 `scripts/verify-macos.sh` 都是同一入口的包装。macOS 设置 `CODESTUDIO_LLVM_ROOT` 指向 LLVM 安装，`CODESTUDIO_LIBCXX_ROOT` 指向 `scripts/build-macos-libcxx.py` 构建的时区运行库，以 `CODESTUDIO_CONTRACT_PYTHON` 选择测试 Python；具体环境与标准库要求见 [macOS 指南](macos.md)。

`--mode all` 按 Debug、Release 顺序运行，不并行修改一个 XMake 缓存。WSL 先通过 `scripts/wsl_sync.py` 同步到独立的 Linux 原生目录。所有 Python 套件均使用启动入口的同一个解释器。默认完整集合包括平台相应的真实隔离构建及包测试；本地模型测试只运行既有合成 HTTP 夹具，不联系 Ollama 或真实模型。

```text
python scripts/verify.py --list
python scripts/verify.py --mode release --preflight-only
python scripts/verify.py --mode release --skip-build --suite schema --suite mcp_http
```

每次调用在 `build/verification/<platform>/<mode>-<UTC时间>-<随机标识>/` 创建独立日志和 `verification.json`，记录源摘要、实际工具链、步骤、返回码、通过/跳过数量、缺省未选套件及二进制 SHA256。失败、中断和仅预检均不标记为通过。`--suite` 是明确的部分验证；`--skip-build` 不证明二进制由本次源码编译，报告保留此信息。入口以锁拒绝同一原生目录的并发验证，源码或测试二进制变更使报告失败；不要在验证期间另开 xmake 修改该目录。`--timeout` 为每一步设置墙钟期限，超时结束该步骤的进程组；已脱离进程组的后台进程和 OS 临时资源仍需检查，报告明确保留这一清理边界。

原生隔离条件缺失会失败，测试自身允许的平台限制会记为跳过。Linux Secret Service 测试只有在可用的测试会话中才能覆盖读写与锁定；无服务时的跳过不等于该能力通过。不要在 CI 中使用个人凭据或让不可信仓库代码进入有权限的 runner。

## CI

[native.yml](../.github/workflows/native.yml) 提供 Windows、Linux 和 macOS ARM64 自托管 runner 作业，要求标签 `codestudio-agent` 及对应系统/架构。作业复用统一入口运行两种配置；Windows/Linux 额外打包并验证临时安装，上传报告和包；macOS 阶段 1 上传原生验证报告。它不发布 Release、不推送代码、不永久安装 Agent。runner 需要预先满足工具链、OS 隔离、网络依赖获取与测试会话条件。

工作流只配置手动触发及 main 分支 push，未为不可信 PR 启用自托管执行。仓库已建立 Git 远端；提交或推送工作流不代表托管 CI 已经通过。CI 的实际状态以对应 runner 的作业日志为准。

macOS 已有原生 arm64 验证入口与 CI 作业定义，本机 Debug/Release 验收通过，尚未提供发布包、安装或签名公证后端。[macOS 指南](macos.md)将用户级安装/升级/卸载、签名、公证和干净 Mac 验收保留在阶段 4；Intel/Universal 包按需求增加。阶段 1 回执会明确跳过原生 OAuth、监督、隔离资源/插件、真实隔离构建与安装；通用协议和失败关闭行为继续验证。不能把这些跳过项或 CI 定义视为验收通过。

## 创建发布包

以下发布打包入口当前仅支持 Windows/Linux。在相应原生平台构建 Release 后执行：

```text
python scripts/package.py --mode release --verification <成功的Release报告>/verification.json
```

`--verification` 可选；提供时必须与当前平台、Release 模式和实际二进制摘要一致。未提供的包不声称附有验证报告；部分报告仍显示原有范围。打包工具不编译、不下载或打包系统库。

默认输出：

| 平台 | 解包目录 | 归档 |
|---|---|---|
| Windows | `build/packages/windows-x64/codestudio-agent-windows-x64/` | `codestudio-agent-windows-x64.zip` |
| Linux | `build/packages/linux-x86_64/codestudio-agent-linux-x86_64/` | `codestudio-agent-linux-x86_64.tar.gz` |

解包目录含可执行文件、安装脚本、协议、示例、配置文档、构建/运行依赖说明、`package.json` 和 `SHA256SUMS`。清单为当前格式 2，不提供历史转换。SHA256SUMS 覆盖清单与每个载荷文件；归档固定顺序、时间戳和权限。内容及元数据相同才保证归档可重复，不宣称不同编译环境产生相同二进制。已有发布目录只有在原所有权和摘要完整时才允许替换。

Linux 包是动态链接的本机构建；必须满足记录中的 glibc、libstdc++、curl、OpenSSL、SQLite、libsecret 等要求，不能将 Arch 包视为任意发行版通用包。隔离执行另需 Bubblewrap、cgroup 和内核配置。Windows 包的实际导入依赖列在 `runtime-dependencies.txt`。校验和证明文件一致性，不证明发布者身份；只安装可信来源的包。

## 安装、升级和卸载

先把归档解到一个独立目录。安装脚本只接受解包目录，不执行归档中的安装钩子。使用 Python 3.11 或更新版本即可覆盖构建及安装工具要求。

```text
python install.py install --package .
python install.py status
```

默认安装到当前用户目录：Windows 为 `%LOCALAPPDATA%\Programs\CodeStudio-Agent`，Linux 为 `~/.local/share/codestudio-agent`。自定义位置使用 `--prefix <目录>`，后续管理命令必须传同一位置。程序路径稳定为 `<prefix>/codestudio-agent.exe` 或 `<prefix>/codestudio-agent`。可选 `install.ps1` / `install.sh` 包装使用相同参数；不需要管理员权限。

安装完成后通过绝对路径运行。例如：

```powershell
& "$env:LOCALAPPDATA/Programs/CodeStudio-Agent/codestudio-agent.exe" doctor
& "$env:LOCALAPPDATA/Programs/CodeStudio-Agent/codestudio-agent.exe" demo --workspace D:/YourWorkspace
```

```bash
"$HOME/.local/share/codestudio-agent/codestudio-agent" doctor
"$HOME/.local/share/codestudio-agent/codestudio-agent" demo --workspace /your/workspace
```

安装不改变 PATH、shell profile、服务、自启或凭据，也不启动 worker。需要自启时另用 [scheduler-autostart-enable](scheduler.md)，使用安装后稳定的程序路径。工作区及状态库应放在安装目录之外。

升级前停止旧程序的全部服务、worker 和 supervisor，再从新包目录执行：

```text
python install.py upgrade --package . --prefix <既有安装目录>
python install.py status --prefix <既有安装目录>
python install.py uninstall --prefix <既有安装目录>
```

`install` 不隐式覆盖不同的包；`upgrade` 明确允许同版本修订和更高版本，拒绝降级。相同包重复安装/升级是无变化操作。所有文件校验后才暂存并发布；安装期间的普通发布失败尝试恢复原目录。清理旧包失败时保留旧目录并在 JSON 结果中返回 `previous_package_retained` 和路径。卸载不存在的安装是无变化操作。

安装工具核对 `.codestudio-install.json` 所有权、原路径、完整文件集合和摘要。发现修改、缺失、额外文件、硬链接、符号链接或 Windows reparse point 时拒绝升级/卸载，保留数据供人工检查，不提供强制删除。修改包后需从可信来源重新取得包；不要手工编辑所有权凭据来绕过检查。

安装操作锁只协调安装脚本。进程检查不能阻止另一个程序在检查后启动 Agent，Linux 也可能无权观察其他会话的进程，所以升级/卸载期间应保持应用停止。暂存失败返回非零状态 `staging_incomplete` 和保留路径；卸载清理中途失败返回 `cleanup_incomplete`，剩余文件保留在独立目录，不冒充完整安装恢复。发布和回滚均失败时返回 `publication_incomplete` 及旧安装备份路径。断电或强杀可能留下唯一命名的 stage、backup 或锁文件；不会猜测删除这些目录。先确认没有活动安装和 Agent 进程，再检查完整清单与旧目录，由管理员恢复或清理；这不是具备断电事务保证的系统包管理器。

## 干净机器与真实登录验收

自动交付测试使用临时安装目录和独立工作区，运行实际的 doctor、离线任务、本地任务服务、后台启动/停止、升级及卸载；不修改全局设置。它证明当前宿主的安装流程，不替代干净 VM 或真实登录测试。

发布候选应在独立测试机器逐项留存输出：

1. 记录系统、用户身份和依赖；在没有源码/构建工具的环境解包、核对 SHA256SUMS，安装并运行 `doctor`、`version`。
2. 在安装目录外运行离线 demo，确认动作回执与文件；开启/停止后台 worker，记录启动、状态与退出。
3. 单独建立测试凭据，验证当前用户可读；授权测试工作区自启，记录真实注销/登录后 worker 身份及该会话的凭据可用性。Linux 要记录用户管理器/linger 条件；WSL 发行版启动不能冒充整机登录。
4. 停止程序，升级并核对工作区数据库和文件保留；关闭自启、删除测试凭据、卸载应用，再确认工作区数据仍在。

本地验证不执行用户注销、重启或永久自启登记。Windows 外层 Job 可能阻止独立后台成功启动，该情形须报告拒绝及无状态变更，不能报告启动成功。真正的干净系统、真实登录、签名/公证与线上分发以单独验收回执为准。
