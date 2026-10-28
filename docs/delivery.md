# 构建、验证与用户级安装

CodeStudio Agent 的运行程序使用 C++26 + XMake。Python 仅用于开发验证、打包和安装管理；已安装的 Agent 本身不需要 Python。包保留原可执行文件名称，用户工作区、SQLite 数据库和系统凭据独立于应用安装目录。

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

[native.yml](../.github/workflows/native.yml) 提供 Windows、Linux 和 macOS ARM64 自托管 runner 作业，要求标签 `codestudio-agent` 及对应系统/架构。作业复用统一入口运行两种配置，再打包并验证临时安装，上传报告和包；macOS 上传的是 ad-hoc 测试包，不自动使用 Developer ID 或公证凭据。它不发布 Release、不推送代码、不永久安装 Agent。runner 需要预先满足对应平台的工具链、OS 隔离、网络依赖获取与测试会话条件。

工作流只配置手动触发及 main 分支 push，未为不可信 PR 启用自托管执行。仓库已建立 Git 远端；提交或推送工作流不代表托管 CI 已经通过。CI 的实际状态以对应 runner 的作业日志为准。

macOS 支持原生 arm64 包和用户级安装；签名公证通过下述独立入口执行，实际结果见[阶段 4 回执](../research/macos-stage4/README.md)。Intel/Universal 包按需求增加。隔离资源/插件与真实隔离构建继续明确跳过；真实登录触发使用单独的会话探针。安装测试通过、CI 定义和 ad-hoc 签名均不等于正式发布验收。

## 创建发布包

在 Windows x64、Linux x86-64 或 macOS arm64 原生平台构建 Release 后执行：

```text
python scripts/package.py --mode release --verification <成功的Release报告>/verification.json
```

`--verification` 提供时必须与当前平台、Release 模式和原始构建二进制摘要一致；部分报告仍显示原有范围。未提供的包不声称附有验证报告。打包工具不编译或下载依赖；macOS 复制已经构建并验证的专用 C++ 运行库，系统库不随包携带。

默认输出：

| 平台 | 解包目录 | 归档 |
|---|---|---|
| Windows | `build/packages/windows-x64/codestudio-agent-windows-x64/` | `codestudio-agent-windows-x64.zip` |
| Linux | `build/packages/linux-x86_64/codestudio-agent-linux-x86_64/` | `codestudio-agent-linux-x86_64.tar.gz` |
| macOS | `build/packages/macosx-arm64/codestudio-agent-macosx-arm64/` | `codestudio-agent-macosx-arm64.tar.gz`（开发测试包） |

解包目录含可执行文件、安装脚本、协议、示例、配置文档、构建/运行依赖说明、`package.json` 和 `SHA256SUMS`。清单为当前格式 2，不提供历史转换。SHA256SUMS 覆盖清单与每个载荷文件；归档固定顺序、时间戳和权限。内容及元数据相同才保证归档可重复，不宣称不同编译环境产生相同二进制。已有发布目录只有在原所有权和摘要完整时才允许替换。

Linux 包是动态链接的本机构建；必须满足记录中的 glibc、libstdc++、curl、OpenSSL、SQLite、libsecret 等要求，不能将 Arch 包视为任意发行版通用包。隔离执行另需 Bubblewrap、cgroup 和内核配置。Windows 包的实际导入依赖列在 `runtime-dependencies.txt`。校验和证明文件一致性，不证明发布者身份；只安装可信来源的包。

macOS 包在 `lib/` 携带时区可用的 libc++/libc++abi，并附带对应许可及其他随包依赖的许可说明。打包仅修改副本，将依赖改为 `@loader_path` 相对路径、去除开发机 RPATH，并拒绝未知的非系统依赖或错误架构。原程序和运行库保留原字节；`build-metadata.json` 分别记录 `source_binary_sha256`、最终 `binary_sha256` 及运行库来源/最终摘要。打包后的程序必须再次通过交付测试，不能只引用重定位前的原生回执。部署目标为 macOS 15.0，已验证的具体系统版本以回执为准。

## macOS 签名与公证

默认 tar.gz 只用于开发测试：重定位后的 Mach-O 使用 ad-hoc 签名以满足本机执行要求，不具有 Developer ID 发布者身份，也没有公证票据。开发分支不启用 hardened runtime；正式 Developer ID 分支要求程序和私有运行库属于同一 Team，并启用 hardened runtime。正式交付使用单独的 DMG 流程，在自有暂存副本上签名，输入包保持不变：

```sh
mkdir -p build/releases
python3 scripts/release-macos.py prepare \
  --package build/packages/macosx-arm64/codestudio-agent-macosx-arm64 \
  --output build/releases/macos-arm64-candidate \
  --identity "$DEVELOPER_ID_SHA1" --team-id "$APPLE_TEAM_ID"

python3 scripts/release-macos.py notarize \
  --release build/releases/macos-arm64-candidate \
  --keychain-profile "$NOTARY_PROFILE"
```

`--identity` 必须是明确选定的 Developer ID Application 证书 SHA-1，`--team-id` 必须与其匹配。签名固定主程序与运行库的 identifier，正式程序使用 hardened runtime，不添加放宽执行保护的 entitlement。签名后重写包内清单、校验和与最终字节摘要，再创建并签名 DMG。Apple 账号凭据预先存入 notarytool profile；脚本只接受 profile 名称，不接受密码或私钥文本。

只有 `notarize` 会上传 Apple。接受结果、日志中的原 DMG 摘要、装订、签名与 Gatekeeper 检查全部成功后才能记录公证成功，并对装订后的最终 `codestudio-agent-macosx-arm64-notarized.dmg` 重新计算 SHA256。原始提交 DMG 保持不变，装订在独立副本完成，失败保留候选；发布中断只按持久化的最终摘要恢复，不重传 Apple。裸 CLI、tar.gz 和 zip 不能冒充已装订的分发容器。提交响应丢失会保留“结果未知”状态并阻止自动重传；取得原 submission ID 后，通过 `--submission-id` 恢复同一提交，不能用另一份已接受记录替代。失败保留证据和待检查产物，不发布 GitHub Release。该恢复流程覆盖已记录的进程中断边界，不宣称突然断电后的文件系统事务持久性。

本地开发可使用 `prepare --ad-hoc` 验证副本、DMG 和失败路径；该产物始终标为 `development_only`，不能进入公证成功状态。准备、正式签名、公证、独立机器验收各自的回执必须对应实际执行，不因脚本存在而标记通过。

## 安装、升级和卸载

先把归档解到一个独立目录。安装脚本只接受解包目录，不执行归档中的安装钩子。使用 Python 3.11 或更新版本即可覆盖构建及安装工具要求。

```text
python install.py install --package .
python install.py status
```

默认安装到当前用户目录：Windows 为 `%LOCALAPPDATA%\Programs\CodeStudio-Agent`，Linux 为 `~/.local/share/codestudio-agent`，macOS 为 `~/Library/Application Support/CodeStudio Agent`。自定义位置使用 `--prefix <目录>`，后续管理命令必须传同一位置。程序路径稳定为 `<prefix>/codestudio-agent.exe` 或 `<prefix>/codestudio-agent`。可选 `install.ps1` / `install.sh` 包装使用相同参数；不需要管理员权限。

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

macOS 安装保留收到的 quarantine 属性，不移除系统下载来源标记、不修改 Gatekeeper 设置。新建安装对象清除继承 ACL，以免将应用文件意外授权给其他主体；既有父目录权限保持原样。APFS 大小写、Unicode 与 firmlink 别名统一到同一安装身份和锁，不能经别名绕过重叠或归属检查。安装工具不再签名或改变 payload 字节。

升级前停止旧程序的全部服务、worker 和 supervisor，再从新包目录执行：

```text
python install.py upgrade --package . --prefix <既有安装目录>
python install.py status --prefix <既有安装目录>
python install.py uninstall --prefix <既有安装目录>
```

`install` 不隐式覆盖不同的包；`upgrade` 明确允许同版本修订和更高版本，拒绝降级。相同包重复安装/升级是无变化操作。所有文件校验后才暂存并发布；安装期间的普通发布失败尝试恢复原目录。清理旧包失败时保留旧目录并在 JSON 结果中返回 `previous_package_retained` 和路径。卸载不存在的安装是无变化操作。

安装工具核对 `.codestudio-install.json` 所有权、原路径、完整文件集合和摘要。发现修改、缺失、额外文件、硬链接、符号链接或 Windows reparse point 时拒绝升级/卸载，保留数据供人工检查，不提供强制删除。修改包后需从可信来源重新取得包；不要手工编辑所有权凭据来绕过检查。

安装操作锁只协调安装脚本。macOS 通过 libproc 观察实际运行镜像身份，发现当前安装正在运行时拒绝升级/卸载，不发终止信号。进程检查不能阻止另一个程序在检查后启动 Agent，Linux 也可能无权观察其他会话的进程，所以升级/卸载期间应保持应用停止。暂存失败返回非零状态 `staging_incomplete` 和保留路径；卸载清理中途失败返回 `cleanup_incomplete`，剩余文件保留在独立目录，不冒充完整安装恢复。发布和回滚均失败时返回 `publication_incomplete` 及旧安装备份路径。断电或强杀可能留下唯一命名的 stage、backup 或锁文件；不会猜测删除这些目录。先确认没有活动安装和 Agent 进程，再检查完整清单与旧目录，由管理员恢复或清理；这不是具备断电事务保证的系统包管理器。

## 干净机器与真实登录验收

自动交付测试使用临时安装目录和独立工作区，运行实际的 doctor、离线任务、本地任务服务、后台启动/停止、升级及卸载；不修改全局设置。它证明当前宿主的安装流程，不替代干净 VM 或真实登录测试。

macOS 包携带仅使用 Python 3.11+ 标准库的 `verify-delivery.py`。从独立工作目录运行，不需要 XMake、LLVM 或源码，也不进行真实账号登录：

```sh
python3 /path/to/extracted/verify-delivery.py --standalone \
  --package /path/to/extracted --receipt /path/to/evidence/new-delivery.json
```

回执记录实际系统、包摘要、测试通过/失败/跳过及 standalone 范围；`clean_machine_verified` 不会自动变为 true。独立 Mac 的来源、没有源码/运行库开发环境的条件、下载 DMG 的 Gatekeeper 接受，以及真实注销/登录仍须由操作者另外留证。不得用开发机改目录运行或 ad-hoc 包测试代替这些验收。

发布候选应在独立测试机器逐项留存输出：

1. 记录系统、用户身份和依赖；在没有源码/构建工具的环境解包、核对 SHA256SUMS，安装并运行 `doctor`、`version`。
2. 在安装目录外运行离线 demo，确认动作回执与文件；开启/停止后台 worker，记录启动、状态与退出。
3. 单独建立测试凭据，验证当前用户可读；授权测试工作区自启，记录真实注销/登录后 worker 身份及该会话的凭据可用性。Linux 要记录用户管理器/linger 条件；WSL 发行版启动不能冒充整机登录。
4. 停止程序，升级并核对工作区数据库和文件保留；关闭自启、删除测试凭据、卸载应用，再确认工作区数据仍在。

本地验证不执行用户注销、重启或永久自启登记。Windows 外层 Job 可能阻止独立后台成功启动，该情形须报告拒绝及无状态变更，不能报告启动成功。真正的干净系统、真实登录、签名/公证与线上分发以单独验收回执为准。
