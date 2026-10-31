# macOS 同步跟进计划：持久请求与交付验收

更新日期：2026-11-01。目标是在 Apple Silicon Mac 上验证最新 schema 8 运行时，并取得对应测试包、真实登录与 CI 的独立证据。本轮执行记录统一见[跟进回执](../research/macos-durable-requests/README.md)及其 M0–M4 状态表。本文固定执行范围和复现步骤，实际通过、排队与待人工操作分别由机器回执记录；计划和入口实现本身不代表验收通过。

## 基线与范围

最新共享实现已提供 `tasks/start` / `tasks/resume` 持久请求去重、`state/initialize` 和 `requests/get`。Windows/WSL Debug/Release 已通过，详见[持久请求验证记录](../research/durable-requests/README.md)。此前 [macOS 原生与交付记录](../research/macos-stage4/README.md) 对应历史源码，不能证明本轮能力通过。

保持 C++26 + XMake、原生 arm64、数据库 schema 8、任务服务及检查点 version 2。旧数据库明确拒绝，不迁移、不删除；试运行使用独立工作区和新状态库。macOS 15.0 是部署目标，历史实测系统为 26.5.2，当前机器与系统版本须重新记录。

本轮继续禁用完整 OS 隔离、插件隔离、隔离工作区执行及资源回收，不引入 Linux 虚拟机替代方案。Developer ID、Apple 公证、独立干净 Mac 验收、Intel/Universal、CodeStudio Next 界面与本地模型开发仍按原计划暂缓。

## 顺序、依赖与状态

| 编号 | 优先级 | 工作 | 前置条件 | 负责执行的角色 | 状态 |
|---|---|---|---|---|---|
| M0 | P0 | 同步完整源码、固定工具链与记录基线 | 已推送 Git 快照、Mac checkout 与工具链可读取 | 源码维护者、Mac 执行者 | 已快进同步 `6e77896`；未直接观察 Windows 当前会话，差异与环境见本轮回执 |
| M1 | P0 | schema 8 原生 Debug/Release 回归 | M0 源码摘要一致、Mac 工具链可用 | Mac 执行者 | 本轮原生与包级执行结果见跟进回执 |
| M2 | P1 | 新测试包及实际归档安装验收 | M1 两配置通过，精确 Release 回执可用 | Mac 执行者 | 本轮原生与包级执行结果见跟进回执 |
| M3 | P1 | 普通/监督模式真实登录自启验收 | M1 通过，固定可运行程序及运行库，GUI 测试会话可用 | Mac 执行者；操作者完成真实注销/登录 | 历史探针未经历新会话；本轮固定运行时与等待/验收状态见跟进回执 |
| M4 | P2 | 运行现有 ARM64 CI 并归档结果 | M0 对应源码已成为可检出的提交，runner 环境就绪 | 仓库维护者、runner 管理者 | 已观察真实 push 作业排队、无 runner 分配；最终提交状态见 CI 回执 |

主线为 M0 → M1 → M2。M3 可在 M1 后与打包工作并行，前提是使用独立固定的运行目录；若选择 M2 解包后的程序，则等待 M2 通过。M4 的环境准备可提前进行，实际作业必须绑定同一提交及源码摘要。各项分别记录完成状态，CI、安装或原生测试通过不代替真实登录结果。

## M0：源码和环境同步

- [x] Mac 已通过 `git pull --ff-only origin main` 同步完整已提交快照 `6e77896`，起始工作区干净。Windows 当前会话不可直接读取，远端 Git 对象与历史 Windows 回执分别记录，不能冒充新的双端实测。
- [ ] 同步完整构建输入，包括新增源码、测试、协议、脚本与文档；不覆盖 Mac 的并行修改，不复制 Windows 的 `build/`、`.xmake/`、数据库、凭据或自启状态。提交/推送另按明确指令执行，文档计划本身不触发发布。
- [ ] 在 M1 冻结前准备好 M2 所需的包级持久请求验收入口及独立夹具路径。若到 M2 才修改测试或夹具，重新冻结源码并生成匹配的 M1 回执后再打包，不引用旧源码报告。
- [ ] 使用 [verify.py](../scripts/verify.py) 的 `source_fingerprint()` 核对远端提交对象与 Mac 构建输入，保存文件数、SHA256、HEAD 与本轮差异清单。若取得 Windows 当前 checkout，再独立计算其摘要；Git 对象比较不证明当前 Windows 工作区状态。文档也是指纹输入，不能沿用旧摘要冒充本次快照。
- [ ] 记录 macOS/SDK、CPU 架构、LLVM、专用 libc++、XMake、Python 与依赖版本。沿用 [macOS 工具链步骤](macos.md#原生构建与验证命令)，核对时区与 `std::format` 运行探针。
- [ ] 在同一扫描范围记录本轮起始进程、自有测试目录、Keychain 测试条目和 LaunchAgent 登记。先检查历史登录探针状态，保留 `build/macos-stage2-login-runtime/` 的固定程序和原证据，不覆盖它们。

完成标准：已推送来源快照与 Mac 同步输入一致，任何不可观察的 Windows 当前状态单列；Mac 工具链与测试用户环境可用；本轮基线和历史探针归属明确。源码未改变前才能进入 M1。

## M1：原生回归

在仓库根目录、已准备好工具链与测试 Python 环境后执行：

```sh
export CODESTUDIO_LLVM_ROOT=/absolute/path/to/llvm
export CODESTUDIO_LIBCXX_ROOT="$PWD/build/toolchains/libcxx-22.1.8-tzdb"
export CODESTUDIO_CONTRACT_PYTHON="$PWD/build/contract-python/bin/python"
bash scripts/verify-macos.sh all --report-dir research/macos-durable-requests/native
```

`all` 顺序构建并测试 Debug、Release，共用缓存不并行写入。`durable_requests` 已在公共套件中，macOS 不跳过该套件。先完成计划与产品文档修改再冻结源码，验证期间只向指纹范围外的 `research/` 写证据；入口会拒绝源码中途变化的回执。运行完整集合后，仅针对失败、修复或新增风险补测；源码发生改变时重新关联受影响回执，保留失败记录。

- [ ] 验证只读初始化不创建数据库，显式初始化使用 schema 8，旧 schema 7 拒绝且文件不被转换。
- [ ] 验证启动与续跑的同键重复请求、并发提交、丢失 accepted、重启查询和会话缓存淘汰；参数、授权、工作区或数据库身份不一致必须拒绝。
- [ ] 验证任务/初始检查点/请求关联原子性、续跑事务、准入写入失败与最终回执写入失败；`incomplete` 不触发重放，未知副作用继续阻断。
- [ ] 在 Darwin 上核对 `/var` 与 `/private/var`、临时目录规范路径、APFS 大小写/Unicode 别名及 POSIX 锁行为。先检查现有夹具覆盖，缺失时增加针对性回归，不能放宽生产身份比较来让测试通过。
- [ ] 复核 Keychain/OAuth、MCP、后台监督、cron 和权限绑定；使用合成服务，普通凭据测试使用随机自有条目并清理，锁定场景使用独立临时 Keychain，不改变用户默认库设置或搜索列表。
- [ ] 用实际 `doctor` 和服务 `initialize` 检查 schema 8、`durable_requests` 声明及禁用隔离能力；记录失败与跳过，不将不支持记为成功。

完成标准：两种配置全部适用套件通过，原生测试组及持久请求用例数量与该源码版本相符（计划基线为 23 组、23 个专项用例；本轮新增 4 个路径、锁与旧库用例后专项为 27 项），每份回执包含实际源码、二进制摘要与工具链。数量不是替代断言和原始日志的验收依据。

## M2：测试包和安装验收

使用 M1 成功的精确 Release 回执。下列 `release_report` 要指向本次运行产生的文件，不能自动选择历史“最新”报告。

```sh
release_report=/absolute/path/to/native/release-run/verification.json
package_output="$PWD/build/packages/macos-durable-requests"
"$CODESTUDIO_CONTRACT_PYTHON" scripts/package.py --mode release \
  --verification "$release_report" --output "$package_output"
package_extract=$(mktemp -d "$PWD/build/macos-durable-delivery.XXXXXX")
tar -xzf "$package_output/codestudio-agent-macosx-arm64.tar.gz" -C "$package_extract"
"$CODESTUDIO_CONTRACT_PYTHON" scripts/verify.py --mode release --skip-build \
  --suite delivery --suite durable_requests --package "$package_extract" \
  --report-dir research/macos-durable-requests/delivery
```

- [ ] 核对包内私有 libc++/libc++abi、许可证、相对依赖路径、arm64 与部署目标，确认无开发机绝对运行库依赖。
- [ ] 保存原构建、重定位/ad-hoc 签名后程序、运行库及 tar.gz 的分别摘要；验证两次相同输入打包的归档一致性，不要求原程序与重定位程序字节相同。
- [ ] 从实际归档解包后验证安装、实际运行、升级、卸载及数据保留。旧状态库拒绝运行与安装程序保留旧数据应分别检查。
- [ ] 补齐包级持久请求验收，覆盖启动/续跑去重、服务重启查询与旧库拒绝，保存包摘要与专项结果的对应关系。若复用 `tests/durable_requests_integration.py`，本轮入口已增加 `--process-fixture` 和 `--mcp-fixture`；`verify.py --suite durable_requests --package` 自动使用所选原生构建的两个夹具，并对测试前后整包和构建产物做摘要检查。默认仍使用程序旁的开发夹具，分发包不携带它们。不得为测试修改已经记录摘要的包载荷。
- [ ] 使用[客户端示例](../examples/task_client.py)验证只读连接、离线任务及持久回执。检查测试后的自有文件、进程及凭据清理情况。

完成标准：新源码对应的测试包、归档安装流程和解包后持久请求测试全部通过，数据保留与旧库拒绝符合契约。本机临时安装不称为独立干净 Mac 验收；ad-hoc 包不称为正式签名或公证发布。

## M3：真实登录验收

- [ ] 先决定现有历史登录探针是完成原版本验收，还是明确取消并通过其 `cleanup` 清理自有登记。旧版本的结果仅归属于原二进制，不升级为 schema 8 证据。
- [ ] 为本轮选择独立固定的可执行程序及完整运行库；可使用 M1 构建的固定副本，或 M2 已通过的完整解包目录。另存运行库清单与 SHA256（探针本身只固定程序及 plist 摘要），核对运行后，直到验收结束不重建、升级、覆盖或删除该目录。
- [ ] 使用新的、不存在的 evidence 目录执行 prepare，记录普通及监督两个 fixture。其间可开展其他工作，但不得改变已登记字节或手动启动 fixture。

```sh
login_runtime=/absolute/path/to/fixed-runtime/codestudio-agent
login_evidence="$PWD/build/macos-durable-login-acceptance"
"$CODESTUDIO_CONTRACT_PYTHON" scripts/verify-macos-login.py prepare \
  --executable "$login_runtime" --evidence-dir "$login_evidence"
# 操作者保存其他工作后，真实注销当前 GUI 用户并重新登录。
# 重新打开终端后恢复原仓库位置、Python 路径和同一个 evidence 路径。
"$CODESTUDIO_CONTRACT_PYTHON" scripts/verify-macos-login.py verify \
  --evidence-dir "$login_evidence"
# 取消验收或需要结束自有探针时：
"$CODESTUDIO_CONTRACT_PYTHON" scripts/verify-macos-login.py cleanup \
  --evidence-dir "$login_evidence"
```

- [ ] 核对 GUI 会话确实改变、原 plist 与程序摘要、launchd 父子关系、监督身份及两种模式的启动结果。
- [ ] 验证禁用登记不会终止已有 worker；显式停止后不被 launchd 重新拉起；清理只作用于本轮自有 fixture。
- [ ] 单独记录登录后测试会话的 Keychain 可用性和必要的后台凭据访问。登录探针本身不代替凭据/OAuth 测试，也不自动解锁用户 Keychain。

完成标准：普通/监督两种模式的真实登录及清理均有机器回执与操作者流程记录。手动 `bootstrap`、同一会话启动或事后进程关系不能单独证明自动登录触发；等待操作者注销/登录时保持“待真实登录”，其他已完成项单独交付。

## M4：CI 实际运行与收尾

- [ ] 准备带 `[self-hosted, macOS, ARM64, codestudio-agent]` 标签的受控 runner，提供匹配 LLVM/时区 libc++、Python/XMake 和测试用户可用 Keychain；不使用个人生产凭据，不执行不可信 PR。
- [ ] 对已同步为提交的源码运行现有 [native.yml](../.github/workflows/native.yml)，核对 checkout SHA、源码摘要、Debug/Release、测试包、实际归档安装与上传产物。此工作流会运行多平台作业，执行前核对所需 runner；不把文件存在当作 CI 通过。
- [ ] 记录实际作业 URL/ID、原始日志、测试结果和产物摘要；失败或排队保持原状态。CI 不执行真实 GUI 注销/登录，不自动签名、公证或发布 Release。
- [ ] 更新本计划状态表、[macOS 指南](macos.md)、[路线图](roadmap.md)、[验证记录](validation.md)和[交付说明](delivery.md)，分别说明原生回归、包、真实登录与 CI 的结果。

完成标准：对应源码的实际 CI 作业和产物可追溯；M0–M4 各自通过或明确标记未完成原因。M3 未通过时，不能宣称 macOS 系统集成阶段整体验收完成。

## 证据与完成记录

本轮新证据统一放入 `research/macos-durable-requests/`，不覆盖 `research/macos-stage*/` 或 Windows/WSL 的历史回执。

| 证据 | 必须记录的内容 |
|---|---|
| `source-sync.json` | 同步方式、两端 HEAD/差异、实际源码文件数与摘要 |
| `environment.json` | 系统/SDK/架构、工具链/运行库、测试会话前提 |
| `native/` | 两配置的原始验证目录、失败修复与补测关联 |
| `package/`、`delivery/` | 源程序/重定位/归档摘要、重复打包、实际安装与解包后专项结果 |
| `login/` | 本轮固定运行时身份、真实登录与自有探针清理结果；必要数据脱敏后的副本 |
| `ci.json` | 对应提交、作业 URL/ID、各项状态和产物摘要 |
| `residue-before.json`、`residue-after.json` | 一致扫描范围内的进程、测试条目、登记与目录；新增残留和历史总残留分开 |
| `completion.json`、`README.md` | 每项结论、证据索引、通过/失败/跳过/未执行、未解决限制 |

只有实际产生的证据才创建相应结果文件，不提前填入成功状态。测试数据、凭据值、OAuth token 和真实用户数据库不进入证据包或 Git。计划执行中需要修复源码时，先保留失败回执，再对实际变更执行适当回归，并重新固定源码与产物关系。
