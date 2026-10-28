# macOS Seatbelt 隔离方案原型

本目录记录 macOS 26.5.2 arm64 上的原生方案验证。**结果是已复现部分隔离效果及关键缺口，不是阶段 3 验收通过，也不启用产品隔离能力。** 夹具仅操作本次创建的文件、进程和临时 IPC 对象；不修改系统安全设置、凭据、用户账户或产品构建缓存。

在仓库根目录复跑，选择一个不存在的工作目录：

```sh
build/contract-python/bin/python research/macos-stage3/seatbelt/reproduce.py \
  --work-dir build/macos-stage3-probes/seatbelt-new-run
```

脚本用 `xcrun clang` 构建 C 夹具和一个独立 dylib，再运行六组有界探针。二进制、scratch、临时宿主文件及报告全部放在指定工作目录。`*.sb` 为模板，`@PROBE_ROOT@` 在运行时替换为该目录。本次完整复跑的原始回执在 [results/reproduction.json](results/reproduction.json)，各用例原始输出也保存在 results 下。三个崩溃场景只结束本次创建的直接子进程；其余后代通过原生 kqueue/出生身份观测确认已经自行退出。

| 边界 | 本机实测结果 | 证据 |
|---|---|---|
| 文件 | deny-default 下 package 可读、不可写；scratch 可写；自有宿主 sentinel 的读取和写入均 EPERM。未隔离对照全部可访问 | [verification.json](results/verification.json) |
| 多文件依赖 | 夹具实际加载 package 内独立 dylib，返回预期值 37 | [verification.json](results/verification.json) |
| 网络 | 未隔离对照能连接本次创建的回环 TCP listener；隔离后 connect 返回 EPERM。Unix socket 同样被拒绝 | [verification.json](results/verification.json)、[ipc.json](results/ipc.json) |
| 同 UID 信号与 IPC | 未隔离对照能向自有目标投递 SIGUSR1、取得 task name port、查找自有 Mach 服务、打开自有共享内存、连接 Unix socket；隔离组全部被拒绝 | [ipc.json](results/ipc.json) |
| task control port | `task_for_pid` 在未隔离与隔离两组都被宿主 OS 拒绝，因此不归功于 Seatbelt | [ipc.json](results/ipc.json) |
| 单进程 | 缺少 process-fork 许可时，fork 和普通 posix_spawn 均被拒绝；外来 executable 的 exec 也被拒绝 | [verification.json](results/verification.json) |
| 进程组逃逸 | 允许 fork 时可通过 setsid 建立新会话；禁止 setsid/setpgid syscall 仍不能阻止 posix_spawn 的 SETPGROUP 参数 | [verification.json](results/verification.json) |
| 单进程自身 exec | 即使禁止 process-fork，POSIX_SPAWN_SETEXEC 仍可保持 PID 并更改 PGID/SID；禁止 SYS_posix_spawn 才阻止该路径 | [setexec.json](results/setexec.json) |
| 宿主退出 | 只用 sandbox-exec 与 POSIX 进程组时，允许的后代能在宿主退出后继续运行 | [host-crash.json](results/host-crash.json) |
| 独立 guardian | guardian 在启动前核对宿主出生身份并绑定 kqueue，持有未 reap 的直接子 PID。宿主被结束后，即使单进程 payload 已用 SETEXEC 改变会话，也能精确终止并 reap 子进程 | [guardian.json](results/guardian.json) |
| guardian 自身失败 | 结束 guardian 后，payload 仍存活，最后由夹具自己的有界 sleep 结束；未冒充产品回收成功 | [guardian-crash.json](results/guardian-crash.json) |
| CPU、内存、聚合进程数量 | 本原型没有提供这类内核资源额度；文件/网络拒绝不能推出这些保证 | 尚未满足完整契约 |

`root.sb` 没有导入 Apple 的 `system.sb`。它明确允许系统运行库、package 以及 scratch，并为本机 dyld/libignition 启动允许读取根目录本身；这不是 `/` 子树读取许可。macOS 26 的 libignition 缺少这一项时会在 main 前终止。允许的系统路径和 syscall/mach bootstrap 规则需要逐 OS 版本重新验证；本次结果不覆盖 macOS 15、Intel 或任意 framework/broker。

单进程 guardian 是可研究的有限机制：禁止后代后，未 reap 的直接子 PID 不会被复用，因而可以精确终止。它不是内核持有的任务组，不能保证 guardian 自身崩溃时也收敛；不能据此宣称普通多进程构建、完整插件包和所有宿主崩溃场景已经支持。禁止 `SYS_posix_spawn` 可以堵住已验证的进程组变化路径，但也会拒绝需要该调用的正常工具；这不能作为透明兼容方案。

## 可维护性与正式能力边界

本机 SDK 的 `usr/include/sandbox.h` 明确写明 “This header is deprecated and may be removed in a future release”，并将 `sandbox_init` 标为 “No longer supported”。`man sandbox-exec` 同样将命令标为 DEPRECATED，指向 App Sandbox。公开头文件只文档化命名 profile，任意 SBPL 的语法与操作不是稳定的公共 API 契约。系统 `dyld-support.sb`/`system.sb` 自身也声明属于 Apple System Private Interface。

App Sandbox 是 Apple 指向的支持方案，但本原型未将其签名/entitlement、继承、每次任务的动态路径及聚合资源额度组合成可接受后端；不能把文档中的替代方案视为本产品已经实现。虚拟机等另一种边界需要独立方案、依赖和原生验收。

因此，本原型可支撑“某些原生隔离操作已证实”的结论，不能支撑完整阶段 3、Windows Job/LPAC 等价或 Linux namespace/cgroup 等价的结论。不满足现有授权/额度/生命周期契约的请求应继续拒绝。
