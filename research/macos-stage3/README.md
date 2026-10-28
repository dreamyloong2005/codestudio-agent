# macOS 阶段 3：文件准备层与隔离候选机制

**阶段 3 尚未完成。** 本轮实现 Darwin 安全文件准备层，并在本机验证了 Seatbelt 和资源限制候选接口。现有插件/构建契约所需的整组物理内存、CPU 百分比、进程数量和崩溃收敛没有全部兑现，因此产品继续拒绝隔离执行与资源回收。原型成功复现预期结果（包括逃逸或存活负例），不等于隔离后端验收通过。

## 已交付的代码

- [Darwin 文件操作](../../src/platform/build_workspace_macos.cpp)：逐组件打开并保持目录描述符，拒绝链接、非普通输入和变化快照；通过目录身份识别 APFS firmlink 根别名，保护路径支持大小写、Unicode 与尚不存在的后缀。
- 安全导入和计量遵循文件数、字节数、目录深度及取消限制；来源根位于保护目录内部时拒绝整个来源，此祖先保护修正也应用到 Linux/Windows 的同形实现。新文件和目录清除继承 ACL，并规范化权限，文件仅保留可执行属性。
- 导出先复制到私有 staging、生成 SHA256 清单，再用 `RENAME_EXCL` 原子发布。清理不跟随链接或跨挂载点，也不改变链接目标的权限。
- [工作区测试](../../tests/build_workspace_tests.cpp)与[进程包测试](../../tests/sandbox_tests.cpp)覆盖这些独立文件操作。包复制通过仍不允许执行包；没有启用 `os_sandbox`、`plugin_sandbox`、`workspace_build.available` 或 `sandbox_resources`。

## 原型证据与后续决策

| 证据 | 已验证的行为 | 仍缺少的保证 |
|---|---|---|
| [Seatbelt](seatbelt/README.md) | 文件、网络、同 UID 信号/IPC 拒绝，包内 dylib 加载，fork 拒绝 | 允许后代或 SETEXEC 时可改变进程组；用户态 guardian 自身退出不收敛负载；公开入口 deprecated、SBPL 非稳定公共接口 |
| [资源限制](limits/README.md) | 普通用户权限下各原生接口的真实返回值和 CPU 采样 | AS/RSS 不是要求的物理额度；NPROC 是 UID 总量；CPU rlimit 是累计秒数；私有策略未构成不可撤销的进程树总额 |
| Virtualization.framework 查询 | 当前主机支持该框架，能够查询 CPU/内存配置范围 | 未创建或启动客体，未验证 Linux cgroup、工具链兼容或生命周期 |

没有把轮询超限后终止、普通进程组或单个 guardian 当作内核执行容器。资源归属登记和精确 ID 回收也未接入：缺少可信的资源成员空状态时，不能凭目录名推断所有权或回收安全。

本轮明确选择保持原生 macOS，暂不启用未满足契约的隔离能力。Linux 虚拟机会改变插件和工具链的执行平台，因此不引入该方案。没有安装 VM、创建客体、请求管理员权限或修改系统安全设置；完整阶段 3 仍需后续原生机制与验收证据。

## 验证与残留范围

[verification.json](verification.json)汇总本轮原生 Debug/Release 回归及源码/产物指纹，逐套件原始记录保存在 `debug/`、`release/`。每个配置通过 22 组原生测试、349 项 Python 测试，11 项 Python 场景明确跳过；共登记 360 项 Python 场景。完整隔离套件继续明确跳过，拒绝行为仍由原生/CLI 测试验证。本轮首个失败回执另存 `failed-attempts/`，本机 `build/verification/macosx/` 也保留原始尝试，不计入最终通过结果；历史阶段 1、2 回执保留其原始语义。

两组原型各自记录源码 SHA256、实际命令、结果和限制；这些研究源文件与结果独立于产品源码指纹。Seatbelt 的崩溃负例用有界夹具自行退出，不能算产品回收成功。原始探针工作目录和编译产物作为研究证据留在 `build/macos-stage3-probes/`，不属于已清理资源。

[residue-audit.json](residue-audit.json)单独记录本轮已知探针及有限范围的只读残留观察。没有运行前完整基线，也没有 macOS 产品资源登记后端，因此历史总残留为未知，不能从当前没有匹配进程推导“全机零残留”。其他平台的历史资源记录继续保留，不由本轮认领或清理。[最终原生夹具检查](native-fixture-audit.json)具有正式 Debug/Release 之前的有限目录基线，三类文件测试夹具新增目录残留为 0；该数字不能推广到所有历史资源或产品回收能力。

阶段 2 的真实登录验收仍待执行。登录夹具使用 `build/macos-stage2-login-runtime/` 中原始 Release 的固定副本，避免本轮重建改变待验收字节；[更新后的准备记录](../macos-stage2/login-acceptance-prepared.json)不表示已经发生真实登录。
