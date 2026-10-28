# macOS 原生资源限制实测

这些探针验证阶段 3 的资源限制候选接口。当前产品要求的是每次插件或构建执行的**整棵进程树**的 CPU 百分比、物理内存字节数和进程数量限制。实测说明，下列接口不能直接等价实现该契约；结果不构成 macOS 原生多进程沙箱能力已完成的依据。

## 复现

在装有 Xcode/Command Line Tools 的 macOS 上，以普通用户运行：

```sh
python3 research/macos-stage3/limits/run.py --output /tmp/macos-limits-results.json
```

脚本只使用 Python 标准库。C 源码以 `clang -Wall -Wextra -Werror -O1` 编译，Swift 源码使用 `xcrun swiftc`。临时可执行文件位于仓库 `build/macos-stage3-probes/limits/run-*`，运行结束自动清理。每个运行案例的外部超时为 5 秒；C 程序还设置了 4 秒 alarm 并禁用 core dump。单次最多触碰 64 MiB 内存、创建一个子进程或一个线程，CPU 采样窗口约 1.1 秒。超时只清理 runner 为该探针新建的进程组。

探针不要求 root、私有 entitlement 或系统配置修改，不操作其他进程。限制只施加给当前探针；fork 子进程会被回收，pthread 会被 join。私有接口仅用于研究，不是产品依赖。脚本拒绝 root 或非 macOS 环境；退出状态 0 说明编译和预期退出路径通过，不能解释为资源隔离能力通过。

`results-macos-arm64.json` 是 macOS 26.5.2 / Darwin 25.5.0 / arm64 / UID 501 上的原始结果，含源码 SHA-256、编译命令、运行命令、退出状态、计时、标准输出与标准错误。实际捕获时间记录在 JSON 中。不同机器的 UID 进程数量、CPU 调度和虚拟地址布局会改变具体读数。

## 结果及边界

| 候选接口 | 本机观测 | 对产品契约的含义 |
| --- | --- | --- |
| `RLIMIT_AS` / `RLIMIT_RSS` | 32 MiB 均返回 `EINVAL (22)`；AS 的 64 MiB、512 MiB、4 GiB 也失败。最小进程虚拟地址空间约 415 GiB，而物理 footprint 约 0.86 MiB。 | macOS 的 RSS 是 AS 的别名。AS 针对虚拟映射大小，设置值低于已有映射时会失败；不能把它直接称为物理内存上限，也不能据此声称 AS 被内核忽略。 |
| `RLIMIT_NPROC` | 设置 soft/hard=2 成功，但探针没有子进程时第一次 `fork` 就返回 `EAGAIN (35)`。 | 计数按 UID 进行，受同用户其他进程影响，不能直接隔离单次插件/构建的进程数。 |
| `RLIMIT_CPU` | soft=1 秒、hard=2 秒设置成功，约 1 秒后以 `SIGXCPU` 退出。 | 这是单进程累计 CPU 时间，单位为秒，不是进程树的 CPU 百分比。 |
| `task_set_phys_footprint_limit` | 自身 32 MiB 限制返回 `KERN_NO_ACCESS (8)`。 | 普通进程未获得所需权限；不能作为当前无特权运行时的物理内存限制实现。 |
| `memorystatus_control`，命令 6 | 自身 32 MiB jetsam task limit 返回 `EPERM (1)`。 | Apple 源码对该操作执行 root / 私有 entitlement 检查。 |
| `proc_setthread_cpupercent` | 当前线程 10% 请求成功，约 1.11 秒消耗 0.18 CPU 秒；新 pthread 在约 1.10 秒内消耗约 1.10 CPU 秒。 | 该策略只限制调用线程；不能据此宣称全任务或全进程树已受限。 |
| `proc_setcpu_percentage` | 自身 20% 请求成功，约 1.10 秒消耗 0.30 CPU 秒。 | 私有接口可以影响当前进程，短采样窗口含突发额度，不能把测量值当作精确稳定百分比。 |
| `proc_clear_cpulimits` / fork | 当前进程能成功清除自身 CPU 限制；新 fork 子进程也未继承上述限额。两种情况下约 1.10 秒消耗约 1.10 CPU 秒。 | 单独使用上述策略不能构成不可撤销的进程树总量限制。探针没有验证通过额外沙箱策略阻止 self-clear 的设计，不应把结果扩展为所有组合方案均不可能。 |
| Virtualization.framework | `VZVirtualMachine.isSupported=true`，允许 CPU 1..64、内存 4 MiB..16 GiB。 | 这仅说明当前主机支持该框架；没有创建 VM、启动 Linux guest、配置 guest cgroup 或验证集成，不能据此启用 VM 沙箱能力。 |

这些程序没有验证整个沙箱的进程树生命周期、不可信 workload 的全部逃逸路径，或 broker 退出后的清理语义。普通进程组和事后监控本身不构成资源安全边界；完整能力仍须对应的执行后端和失败路径测试支撑。

## 参考依据

`references.json` 列出 Apple 官方 XNU 源码链接、快照标识、Git blob SHA-1 和下载内容 SHA-256。引用内容已核对 Git blob 哈希；此快照用于解释接口语义，不宣称与本机 Darwin 内核源码版本完全一致。本机运行证据由上述 JSON 单独记录。

- `bsd/kern/kern_resource.c`：AS 的 `vm_map_set_size_limit`、NPROC 的每 UID 语义、线程 CPU 限制分支。
- `osfmk/kern/task.c`：`task_set_phys_footprint_limit` 调用 `proc_check_footprint_priv`。
- `bsd/sys/kern_memorystatus.h`、`bsd/kern/kern_memorystatus.c`：命令 6、MB 参数位置和权限检查。
- `libsyscall/wrappers/libproc/libproc.c`、`libproc_private.h`：三个私有 CPU 接口的准确函数签名。
- `bsd/sys/process_policy.h`、`bsd/kern/process_policy.c`、`osfmk/kern/task_policy.c`：THROTTLE 动作和 CPU ledger 实现。
- 本机 SDK 的 `sys/resource.h` 定义 `RLIMIT_RSS` 为 `RLIMIT_AS`，`mach/task.h` 声明 footprint API。SDK 路径记录在运行结果中。
