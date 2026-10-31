# CodeStudio Agent macOS schema 8 跟进

本目录记录 2026-11-01 的 macOS 后续计划执行。来源为远端提交 `6e77896`，本轮在 Mac 上增加包级持久请求入口、Darwin 路径/锁及旧库拒绝专项，并将 CI 的包级测试绑定当前 run/attempt 的原生回执。生产授权比较及隔离能力未放宽。

实际完成范围、源码与包摘要以 [completion.json](completion.json) 为准。M0 同步和环境、M1 原生 Debug/Release、M2 实际归档与包级持久请求分别记录。真实登录需要操作者完成 GUI 注销/登录；CI 缺少可用 runner，用户选择先完成本机验证并记录排队状态。正式签名、公证、独立干净 Mac、Intel/Universal 与 Linux 虚拟机继续暂缓。

| 项目 | 本轮结果 | 证据 |
|---|---|---|
| M0 | 已同步已推送 Git 快照并记录本轮改动；Windows 当前会话未直接观察 | [源码](source-sync.json)、[环境](environment.json) |
| M1 | Debug/Release 各 23 组原生测试、407 项 Python 通过、11 项跳过；每配置持久请求 27/27 | [Debug](native/debug-20261031T174514Z-3cadb9c3/verification.json)、[Release](native/release-20261031T174734Z-0d7e5aba/verification.json) |
| M2 | 相同输入的两次归档字节一致；实际解包交付 35/35、持久请求 27/27；安装客户端、重启查询、旧库拒绝与数据保留通过 | [归档](package/packaging.json)、[交付](delivery/release-20261031T174955Z-22bb05b1/verification.json)、[客户端](installed-client/verification.json) |
| M3 | schema 8 完整运行时已固定，普通/监督两个探针已登记、未启动；待操作者真实注销/登录 | [运行时](login/runtime-manifest.json)、[登记](login/prepared.json) |
| M4 | 用户确认暂无 runner；记录实际作业排队，未取得执行或产物验收 | [CI 预检](ci-preflight.json)，最终提交快照在 `ci.json` |

最终测试源码为 227 文件，SHA256 `6a7cf041e668e31b3c1f134003242fe7e1745f014abb6d615f994fbe73e6304c`。开发归档为 `build/macos-durable-requests/package-1/codestudio-agent-macosx-arm64.tar.gz`，SHA256 `a4cbd76490e7ff8995b99d52057eacc40d72580385db091ad930b00b0b1bbc25`。完整包内程序与两份私有运行库分别留有来源和最终摘要。

首次 Debug 回归中，新增加的两个测试预期与现有行为不符：Darwin 已将 APFS 大小写/Unicode 别名规范到同一工作区；schema 7 数据库则在 `serve` 开始接收请求前被拒绝。原始失败记录保留在 `native/debug-20261031T174223Z-a792c05d/`，修正测试后重新冻结源码并执行完整回归，不把失败快照算作最终验收。

`source-sync.json` 比较来源 Git 对象和本机输入，Windows 当前会话不可读取；历史 Windows 测试不表示本轮重新运行。`environment.json` 记录工具链预检，实际能力与依赖版本由原生回执及包运行探针确认。源码指纹涵盖文档、测试、脚本，不包含本目录；本轮研究回执更新不会替换已测试源码。

残留观察只覆盖声明范围。`residue-before-initial-bound.json` 保留第一次 TMPDIR 枚举上限不足；后续完整基线在产品测试开始前采集。Keychain 只读取两个产品服务的测试账号属性，不读取 secret data、不修改默认库或搜索列表。历史登录探针单独保留，新探针的待登录登记不是清理遗漏。

[同范围对比](residue-comparison.json) 没有发现新增测试临时目录、Keychain 测试项、OAuth lock 或 Agent/fixture 进程；两个历史与两个本轮待登录登记均未加载。固定包的 52 个文件及历史程序/运行库摘要保持一致。

## 真实登录的续接

保留 `build/macos-durable-requests/extracted/`、历史 `build/macos-stage2-login-runtime/` 及它们的运行库，验收结束前不得覆盖或删除。操作者保存其他工作后真实注销当前 GUI 用户并重新登录，不手动 bootstrap 或启动探针；返回本聊天确认操作后，分别验证本轮 schema 8 与历史 schema 7：

```sh
build/contract-python/bin/python -B scripts/verify-macos-login.py verify \
  --evidence-dir build/macos-durable-login-acceptance
build/contract-python/bin/python -B scripts/verify-macos-login.py verify \
  --evidence-dir build/macos-login-acceptance
```

验证前重新核对 `login/runtime-manifest.json` 与历史回执的程序、私有运行库摘要。`verify` 检查新会话、launchd 归属、普通/监督身份，并验证禁用登记保留 worker、显式停止后不重启。成功后还须记录登录会话下的合成 Keychain/OAuth 测试和同范围清理；旧版本结果不能记为 schema 8 结果。若取消，分别对自己的 evidence 目录执行同一工具的 `cleanup`，不删除未知登记。

本机运行只证明实际 macOS 版本与 arm64，macOS 15.0 是部署目标。开发 tar.gz 的相同输入可比较归档字节；ad-hoc 签名不证明发布者身份或 Apple 公证。真实登录、远端 CI、独立机器的缺口不能由本机测试数量替代。
