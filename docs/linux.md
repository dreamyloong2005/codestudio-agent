# Linux 与 WSL

Linux 后端使用 C++26、XMake、libcurl、SQLite、OpenSSL、libsecret 和 nlohmann-json。当前验证平台为 Arch Linux x86-64 / WSL2，编译器 GCC 16；验证结果见 [validation.md](validation.md)。macOS 与其他架构尚未验证。

## 构建

安装 GCC、XMake、Python 3、pkgconf、curl、SQLite、OpenSSL、libsecret、nlohmann-json、tzdata；运行隔离任务还需要 Bubblewrap。Arch 的软件包名分别为 `gcc xmake python pkgconf curl sqlite openssl libsecret nlohmann-json tzdata bubblewrap`。凭据后端通过 pkg-config 的 `libsecret-1` 获取头文件及 GLib/GIO 依赖；即使其他库交由 XMake 获取，libsecret 仍使用系统开发包。依赖应从发行版签名仓库安装。

WSL 下建议把源码同步到 Linux 文件系统，避免 Windows 缓存、权限和路径语义影响构建。在 PowerShell 执行：

```powershell
wsl -d archlinux -- python3 /mnt/d/Dev/Projects/codestudio-agent/scripts/wsl_sync.py /var/tmp/codestudio-agent-linux
wsl -d archlinux -- sh -lc 'cd /var/tmp/codestudio-agent-linux && XMAKE_ROOT=y xmake f -p linux -a x86_64 --toolchain=gcc -m release --system_deps=y -y && XMAKE_ROOT=y xmake build -a -j 6'
```

发行版名称和路径按本机调整。`XMAKE_ROOT=y` 仅用于当前 WSL 默认用户为 root 的命令；普通用户不需要它。同步脚本只更新所属镜像中的源码，不复制 Windows `.xmake`、构建产物或数据库，也不删除目标文件。改源码后需要重新同步。

原生 Linux 可直接在源码目录配置、构建。`--system_deps=y` 通过 pkg-config 使用系统开发包；未启用时继续使用 XMake 包依赖。可执行文件位于 `build/linux/x86_64/{debug,release}/codestudio-agent`。

```bash
build/linux/x86_64/release/codestudio-agent doctor
build/linux/x86_64/release/codestudio-agent demo --workspace /tmp/codestudio-demo
```

发行版依赖模式生成动态链接程序；分发时须提供相容的 glibc、libstdc++、SQLite、curl、OpenSSL、libsecret 及其 GLib/GIO 等传递依赖，不能把单个二进制称为跨发行版静态包。libsecret 运行库是二进制依赖；Secret Service 服务提供者只在使用系统凭据时需要。

标准包由 `python scripts/package.py --mode release` 在 Linux 原生镜像生成到 `build/packages/linux-x86_64/`，完整安装流程见 [用户级交付](delivery.md)。保留的 Linux Release 交付目录为 Windows 工作区的 `build/linux/x86_64/release/`。其中 `codestudio-agent-linux-x86_64.tar.gz` 保留原名可执行文件 `codestudio-agent`，并包含独立运行说明、构建机动态库清单、`SHA256SUMS`、客户端协议 Schema 和 Python 客户端示例；当前交付和校验范围见 [验证记录](validation.md)。建议把压缩包解到 Linux 文件系统再运行，使用 `sha256sum -c SHA256SUMS` 校验。归档不包含发行版依赖，也不会自动注册后台任务、自启项或密钥环服务。

在本机 PowerShell 中直接检查已交付的二进制：

```powershell
wsl -d archlinux -- /mnt/d/Dev/Projects/codestudio-agent/build/linux/x86_64/release/codestudio-agent doctor
```

## 系统凭据与后台会话

模型任务可选择 `--credential system:work-model`，由 libsecret 访问当前用户会话的 Secret Service；`env:VARIABLE` 仍读取进程环境。系统库值在每次任务或续跑开始时重新读取，因此后续 job 可使用轮换后的密钥，正在执行的操作保持原值。

需要已有、可读取的默认集合以及正确的会话总线。Agent 不创建或解锁集合、不显示提示、不保存解锁密码，后端调用使用 5 秒整体期限；无服务或默认集合为 `unavailable`，锁定为 `locked`，没有明文回退。WSL 并不保证预装或启动 Secret Service。systemd 用户管理器、终端和后台 worker 的总线环境可能不同，应在实际运行用户和会话下检查凭据状态；自启登记不会代为配置密钥环、D-Bus 或 linger。

管理 CLI、无回显输入示例、缺失状态和权限边界见 [模型凭据与轮换](credentials.md)。libsecret 只负责访问系统存储，不阻止同 UID 的已授权未隔离进程调用同一凭据 API。

## 隔离环境

普通工具和 stdio MCP 使用 `--sandbox linux-namespace`。远程 MCP 无法满足本地进程沙箱授权，该组合会在连接前拒绝。工作区构建固定使用 `linux-namespace-build-v2`，无需指定普通工具沙箱选项。请求隔离但缺少条件会失败，不会降为宿主执行。

后端要求 Linux x86-64、允许用户命名空间的内核、`/usr/bin/bwrap`、seccomp、可写的 cgroup v2 委派目录，以及已在该目录的 `cgroup.subtree_control` 启用的 `cpu memory pids`。宿主环境变量 `CODESTUDIO_CGROUP_ROOT` 可指定委派目录；未指定时尝试当前进程的 cgroup。此变量不进入配方、不持久化为模型授权，也不传给沙箱子进程。

正常运行应由管理员或 systemd 为实际用户配置最小委派，例如受管的 `Delegate=cpu memory pids` 单元；本程序不会自动改全局控制器、创建用户或启用 linger。普通桌面终端的 cgroup 可能不满足条件。`doctor` 只报告编译支持，运行任务时才核验当前环境。

每次会话使用独立用户、挂载、PID、网络等命名空间，丢弃 capabilities；seccomp 拒绝网络 socket、挂载、namespace 切换和进程注入等操作。普通单程序模式禁止直接创建子进程；锁定插件包通过 `sandbox_limits.processes` 明确授权进程预算，默认 1，允许范围为 1..32，值大于 1 才开放受限制的派生。Linux 的 cgroup 任务数包括线程，并在插件预算外预留两个 Bubblewrap 监督槽；默认模式也不允许新线程。构建模式沿用原有 `process_limit`，其计数包括监督进程，至少为 3。

CPU 配额、内存/交换限制和任务数限制在会话独立子 cgroup 中设置；`cpu_percent` 是单个 CPU 的百分比。插件的默认值为内存 512 MiB、CPU 80%，可明确设置 64..4096 MiB 和 1..100%。取消、超时和结束会回收整个进程树。宿主被 SIGKILL 时命名空间后代仍被回收，但空 cgroup 和私有暂存文件可能保留，不能把这些文件当作可续跑检查点。

私有根仅包含明确声明的程序文件、必要系统 ELF 库、最小 `/dev`、PID 命名空间 `/proc` 和本次工作目录。普通程序仅复制 executable；`plugin_lock` 插件则按冻结文件集合复制整个载荷，复核内容摘要和执行模式，并只读映射到 `/tools`。`${plugin}` 参数重写到该副本；独立 `/work` 用作 cwd/home/temp，不能通过它读取或回写宿主工作区。工作区构建才按自己的清单导入源码并导出产物；导入和导出拒绝符号链接、硬链接等非普通输入，导出核验后原子发布。

运行库依赖通过解析 ELF 元数据复制，不执行 `ldd`，也不挂载宿主 `/usr`。锁定插件包的入口须为原生 x86-64 ELF；后端扫描已核验副本中的 ELF 程序和共享库，优先解析包内 `DT_NEEDED`，再从受信的标准系统目录复制缺失依赖。包内 ELF basename 必须唯一，避免依赖选择歧义；`LD_LIBRARY_PATH` 仅包含本包 ELF 所在目录，不继承宿主设置。显式声明并随包复制的私有库可通过依赖或隔离后的绝对路径加载；未声明的运行时模块不会从宿主自动补齐。

脚本须由显式打包的 ELF 解释器运行，解释器的数据目录、扩展模块及其他运行时文件也须完整声明；这不表示任意 Python/Node 发行包均已验证。系统库与解释器运行要求仍需在目标发行版验收。构建清单仍须显式声明编译器、辅助工具、头文件、静态库等输入；不会自动暴露整个宿主工具链。插件示例及跨平台限制见 [插件包](plugins.md)。

此边界依赖宿主内核，不是虚拟机。宿主文件工具和 provider HTTP 保持各自宿主权限；不抵御恶意同 UID 宿主进程、管理员或内核漏洞。尚未实现 OS 磁盘容量配额。取消不能中断每个同步文件复制调用。

## 验证

已有委派的普通用户可以执行：

```bash
python3 -m venv build/contract-python
build/contract-python/bin/python -m pip install -r tests/requirements.txt
export CODESTUDIO_CONTRACT_PYTHON="$PWD/build/contract-python/bin/python"
CODESTUDIO_CGROUP_ROOT=/sys/fs/cgroup/<delegated-group> bash scripts/verify-linux.sh debug
CODESTUDIO_CGROUP_ROOT=/sys/fs/cgroup/<delegated-group> bash scripts/verify-linux.sh release
```

仅供开发验证的 root 辅助脚本会创建唯一临时子 cgroup，在其内部启用已有控制器，执行命令后清理。它要求全局控制器已配置，不会修改系统全局委派。可在同一次 WSL 调用中完成，避免 WSL 空闲退出导致临时 cgroup 消失：

```bash
XMAKE_ROOT=y python3 scripts/linux_test_delegation.py -- bash scripts/verify-linux.sh debug
python3 scripts/linux_test_delegation.py --uid 65534 -- build/linux/x86_64/debug/agent-tests
```

第二条使用现有 nobody UID 验证权限边界，不创建账号。HTTP 测试均使用临时模拟服务，不访问在线模型或本机 Ollama。自启测试使用临时配置目录，不登记开发工作区；systemd 行为和生命周期见 [scheduler.md](scheduler.md)。

`CODESTUDIO_CONTRACT_PYTHON` 只选择协议契约测试所用的 Python；其开发依赖为 JSON Schema 校验器，不是 Agent 的运行依赖。未指定时使用 `python3`，该环境需已安装 `tests/requirements.txt`。
