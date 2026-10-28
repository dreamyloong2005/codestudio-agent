# 隔离资源登记与回收

沙箱运行时为新资源建立当前用户私有的归属登记。登记与任务数据库独立，记录资源 ID、执行种类、工作区、可用时的任务 ID、宿主 PID 和进程出生身份，以及暂存目录和平台资源的原生身份。普通进程、锁定插件包和工作区构建使用同一套管理接口。

MCP 发现可早于任务创建，这类连接的 `task_id` 为 `null`，不伪造任务关联。任务 ID 仅供诊断；是否能够回收取决于租约、进程组和资源身份，不能凭任务结束或目录名称判定。

## 查询与预览

```text
codestudio-agent sandbox-resources --limit 50
codestudio-agent sandbox-resources --after <上一页资源ID> --limit 50
codestudio-agent sandbox-reclaim --id <资源ID> --dry-run
```

列表按 32 位小写十六进制 ID 排序，`limit` 为 1–100，单页条目序列化总量最多 512 KiB，`next_after` 为下一页游标或 `null`。查询和预览不创建登记目录，不删除资源，也不初始化任务数据库。观察结果只反映查询时刻，显式回收会重新检查。

| 状态 | 含义 |
|---|---|
| `active` | 仍有活动租约或匹配的宿主进程，不能回收 |
| `reclaimable` | 当前检查满足回收条件，预览没有执行删除 |
| `blocked` | 身份不匹配、进程组仍有成员、权限或观察不足，保留资源 |
| `invalid` | 列表发现无法解释的登记，保留原文件供检查 |

登记只含资源诊断信息，不保存模型密钥、插件环境或用户提示词。宿主文件工具与隔离构建导入保护登记目录；它也不会映射进子进程沙箱。

## 显式回收

```text
codestudio-agent sandbox-reclaim --id <资源ID>
```

每次只处理一个准确登记的 ID。回收器必须取得排他租约，确认原宿主已退出或出生身份已不匹配，再确认所有 Job/cgroup 已为空或不存在。它不终止 PID，不调用 cgroup kill，也不把进程观测当成重试任务的授权。

删除前校验已登记目录及父路径的原生身份，使用不跟随链接的操作。路径被替换、仍有进程或无法证明归属时拒绝回收。资源载荷清理阶段的部分失败或取消会保留登记，下一次请求重新检查剩余资源；只有确认资源都不存在后才移除登记。

最后删除登记文件、锁文件和控制目录不是一个原子事务。若进程在这个收尾窗口被强杀，可能留下无法自动回收的 `invalid` 登记残片；外部资源此前已确认清空。这一情况会保留给宿主检查，不推断归属或自动修复记录。

成功返回 `reclaimed`；已经没有该登记返回 `absent`。取消返回 `cancelled`，部分执行不能完成时返回 `incomplete`。CLI 对 `reclaimed`、`absent` 和预览的 `reclaimable` 返回退出码 0；拒绝、取消或不完整返回 1。非法参数、列表查询不能读取控制目录返回 2；回收因权限或路径问题无法确认归属时返回 `blocked` 和退出码 1。

回收不更新 SQLite 任务、动作回执或检查点。崩溃留下的 `unknown` 效果仍保持未知，不能因此自动重放工具或续跑任务。目录清理也不撤销此前已经产生的外部效果。

## 生命周期与平台

创建资源前先写入准备意图，创建后记录目录身份，完成准备后标记可启动。登记租约贯穿准备、运行和正常销毁。若宿主在创建与身份登记之间崩溃，存在但没有捕获身份的资源会被阻止回收，不能把这个窗口当作可安全认领的证据。

Windows 使用当前用户私有 ACL、目录文件 ID、命名 Job 和 AppContainer profile 身份；Linux 使用当前用户私有目录、文件锁、目录身份、进程出生身份及 cgroup 的 populated 状态。正常结束仍由原沙箱执行清理，登记接口用于观察与处理未完成的生命周期。

默认控制目录位于 Windows 用户 Profile 下的 `.codestudio-agent/sandbox-resources`，Linux `$XDG_STATE_HOME/CodeStudio-Agent/sandbox-resources`，未设置时使用 `$HOME/.local/state/CodeStudio-Agent/sandbox-resources`。Windows 使用 Profile 目录以保持从普通宿主或 MSIX 宿主启动时的路径稳定。`CODESTUDIO_SANDBOX_REGISTRY` 可指定独立控制目录，适用于测试或宿主配置；目录仍需满足平台的所有权、权限和路径检查。子沙箱不继承此变量。查询另一个配置的登记需要使用相同控制目录。

仅识别本实现写入的当前格式 2 登记，不扫描前缀后认领旧目录，也不导入旧状态。历史未登记的暂存目录以及此前被执行策略拒绝清理的夹具不属于此命令的回收目标。对同用户恶意宿主、管理员或内核攻击不作隔离承诺。

## 任务服务

`initialize.capabilities.sandbox_resources` 声明编译支持。可信本地客户端可调用：

- `sandboxes/list`：参数为可选 `after`、`limit`，同步只读查询，可在其他前台操作期间调用。
- `sandboxes/reclaim`：参数为必填 `id`、可选布尔 `dry_run`，返回异步操作 ID，可用现有操作查询和取消接口跟踪。

接口作用于当前用户控制目录，不局限于服务工作区，也不向模型注册回收工具。无请求 ID 的通知不会触发回收。协议版本保持 2，任务数据库保持 schema 7。完整契约见[任务服务](task-service.md)与[机器协议](../protocol/task-service.schema.json)。
