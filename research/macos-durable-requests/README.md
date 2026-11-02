# CodeStudio Agent macOS schema 8 跟进

本目录记录 2026-11-01 的 macOS 后续计划执行及 2026-11-02 完成的真实登录验收。来源为远端提交 `6e77896`，本轮在 Mac 上增加包级持久请求入口、Darwin 路径/锁及旧库拒绝专项，并将 CI 的包级测试绑定当前 run/attempt 的原生回执。生产授权比较及隔离能力未放宽。

实际完成范围、源码与包摘要以 [completion.json](completion.json) 为准。M0 同步和环境、M1 原生 Debug/Release、M2 实际归档与包级持久请求、M3 真实登录验收均已完成本机验证。CI 缺少可用 runner，用户选择先完成本机验证并记录排队状态。正式签名、公证、独立干净 Mac、Intel/Universal 与 Linux 虚拟机继续暂缓。冻结源码文档中的待登录描述属于准备时快照，最新状态以本目录回执为准。

| 项目 | 本轮结果 | 证据 |
|---|---|---|
| M0 | 已同步已推送 Git 快照并记录本轮改动；Windows 当前会话未直接观察 | [源码](source-sync.json)、[环境](environment.json) |
| M1 | Debug/Release 各 23 组原生测试、407 项 Python 通过、11 项跳过；每配置持久请求 27/27 | [Debug](native/debug-20261031T174514Z-3cadb9c3/verification.json)、[Release](native/release-20261031T174734Z-0d7e5aba/verification.json) |
| M2 | 相同输入的两次归档字节一致；实际解包交付 35/35、持久请求 27/27；安装客户端、重启查询、旧库拒绝与数据保留通过 | [归档](package/packaging.json)、[交付](delivery/release-20261031T174955Z-22bb05b1/verification.json)、[客户端](installed-client/verification.json) |
| M3 | schema 8 普通/监督两种模式真实登录及清理通过；历史 schema 7 独立通过；登录后凭据/OAuth 41 项通过、3 项跳过 | [schema 8](login/accepted.json)、[历史 schema 7](login/historical-schema7-accepted.json)、[验收核对](login/post-login-check.json)、[凭据](login/post-login-native/release-20261102T070848Z-c29c2937/verification.json) |
| M4 | 用户确认暂无 runner；实现提交 `a61283b` 已触发真实运行，快照为 `pending`，未取得执行或产物验收 | [CI 快照](ci.json)、[Actions 运行](https://github.com/dreamyloong2005/codestudio-agent/actions/runs/41226587274) |

最终测试源码为 227 文件，SHA256 `6a7cf041e668e31b3c1f134003242fe7e1745f014abb6d615f994fbe73e6304c`。开发归档为 `build/macos-durable-requests/package-1/codestudio-agent-macosx-arm64.tar.gz`，SHA256 `a4cbd76490e7ff8995b99d52057eacc40d72580385db091ad930b00b0b1bbc25`。完整包内程序与两份私有运行库分别留有来源和最终摘要。

实现与本机证据先提交为 `a61283b` 并推送，以取得绑定该提交的实际 CI 快照。后续提交只补充本目录的 CI/登录验收/完成记录，以 `[skip ci]` 避免为纯证据更新重复排队；测试源码和交付包保持上述摘要。M4 保留先前观察到的 `pending` 快照，本次没有重新查询 CI，也不将其算作执行通过。

首次 Debug 回归中，新增加的两个测试预期与现有行为不符：Darwin 已将 APFS 大小写/Unicode 别名规范到同一工作区；schema 7 数据库则在 `serve` 开始接收请求前被拒绝。原始失败记录保留在 `native/debug-20261031T174223Z-a792c05d/`，修正测试后重新冻结源码并执行完整回归，不把失败快照算作最终验收。

`source-sync.json` 比较来源 Git 对象和本机输入，Windows 当前会话不可读取；历史 Windows 测试不表示本轮重新运行。`environment.json` 记录工具链预检，实际能力与依赖版本由原生回执及包运行探针确认。源码指纹涵盖文档、测试、脚本，不包含本目录；本轮研究回执更新不会替换已测试源码。

残留观察只覆盖声明范围。`residue-before-initial-bound.json` 保留第一次 TMPDIR 枚举上限不足；后续完整基线在产品测试开始前采集。Keychain 审计只读取两个产品服务的测试账号属性，不读取 secret data、不修改默认库或搜索列表。历史登录探针独立验收，原准备与待登录快照继续保留。

[登录前对比](residue-comparison.json) 记录四个待登录登记当时均未加载。[登录后对比](login/residue-comparison.json) 确认四份自有 plist 已移除，未发现新增测试临时目录、Keychain 测试项、OAuth lock 文件或 Agent/fixture 进程。当前会话中四个 launchd 记录均已停止且运行次数仍为 1；空 OAuth 锁目录、合成工作区和固定运行时作为证据保留。固定包的 52 个文件及历史程序/运行库摘要保持一致。

## 真实登录验收结果

操作者先表示会注销并重新登录，之后在本聊天确认“已重新登录”。验证观察到 Aqua GUI 会话 ASID 从 `100024` 变为 `100028`，boot session UUID 也发生变化。Agent 未手动 bootstrap 或启动任何探针。验收前后核对完整包与历史运行库摘要，分别执行：

```sh
build/contract-python/bin/python -B scripts/verify-macos-login.py verify \
  --evidence-dir build/macos-durable-login-acceptance
build/contract-python/bin/python -B scripts/verify-macos-login.py verify \
  --evidence-dir build/macos-login-acceptance
```

两份回执均为 `passed`：普通 worker 与监督进程均是 launchd 的直接子进程，监督 worker 归属正确；每个探针仅启动一次、generation 为 1。禁用登记未终止或替换 worker，显式停止后 launchd 未重新拉起。历史 schema 7 结果仅归属于历史二进制。

随后在同一登录会话对原生 Release 执行 `credentials` 和 `mcp_oauth` 定向验证，源码和产物摘要与此前 Release 回执一致：凭据 15 项通过、3 项按平台/隔离条件跳过；OAuth 26/26。通过项包含启动器退出后 detached worker 读取轮换 Keychain 凭据。测试只使用合成凭据和 loopback 服务，没有自动解锁用户 Keychain；这些结果属于原生 Release，不能称为包级凭据验收。原始日志与跳过原因保存在[后登录测试目录](login/post-login-native/release-20261102T070848Z-c29c2937/)。

本机运行只证明实际 macOS 版本与 arm64，macOS 15.0 是部署目标。开发 tar.gz 的相同输入可比较归档字节；ad-hoc 签名不证明发布者身份或 Apple 公证。M3 已有真实登录证据，远端 CI 和独立机器的缺口仍不能由本机测试数量替代。
