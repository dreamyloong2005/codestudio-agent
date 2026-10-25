# 完整插件包隔离

已安装并锁定的 MCP 插件可在 Windows LPAC 或 Linux namespace 中运行整个包。宿主先核验锁和安装内容，再把声明载荷导入私有目录并检查摘要、大小与 Linux 执行属性；检查完成后才启动入口程序。包内程序、DLL/SO、脚本和配置对沙箱只读，可写 scratch 与原工作区分离。安装包不会自动获得工具或进程权限。

## 使用

先按[插件分发](plugins.md)创建包、安装并生成锁。插件描述符仍使用 `${plugin}/relative/path` 引用包内参数；使用隔离模式时参数自动指向本次只读副本。

```json
{
  "version": 2,
  "servers": [{
    "id": "helper",
    "plugin_lock": "D:/Plugins/helper.lock.json",
    "tools": ["inspect"],
    "timeout_ms": 10000,
    "sandbox_limits": {
      "processes": 2,
      "memory_mb": 512,
      "cpu_percent": 50
    }
  }]
}
```

```text
codestudio-agent mcp-tools --workspace <工作区> --mcp-config <清单> --allow-mcp helper --sandbox appcontainer
codestudio-agent mcp-tools --workspace <工作区> --mcp-config <清单> --allow-mcp helper --sandbox linux-namespace
```

选择当前平台对应命令。`run`、`resume` 和任务服务使用相同的沙箱选项、清单与显式 MCP 授权。`sandbox_limits` 仅适用于 `plugin_lock`，已授权服务器提供此字段但未选择 OS 沙箱时拒绝运行。未授权服务器不加载包、不启动进程。

| 限额 | 默认 | 范围 | 含义 |
|---|---:|---:|---|
| `processes` | 1 | 1–32 | Windows 为整个 Job 活跃进程；Linux 为进程/线程任务数，另预留两个 Bubblewrap 监督槽 |
| `memory_mb` | 512 | 64–4096 | 整个隔离进程组的内存上限 |
| `cpu_percent` | 80 | 1–100 | Windows Job CPU rate；Linux cgroup 配额相当于一个 CPU 的该百分比 |

默认禁止创建后代；Linux 默认也禁止线程。只有宿主清单明确增加 `processes` 才允许受限派生。Windows 多进程包需要额外的 `registryRead` 系统能力以完成子进程初始化，仍不授予网络能力。普通 `--allow-process` 和未锁定 MCP `command` 不继承包的限额或派生权限。

## 文件与依赖

Windows 将包导入临时 profile 下的私有只读 `bin`，Linux 映射为只读 `/tools`；工作目录、HOME 和临时目录指向独立 scratch。相对配置路径以 scratch 为基准，需要包内文件时应使用 `${plugin}/...` 或由程序依据自身位置查找。

Windows 入口必须为 `.exe`，正常 DLL 搜索及显式加载只能访问沙箱可读的文件。被导入的私有 DLL 应放在入口程序同目录；显式加载嵌套 DLL 不保证 LPAC 会把该子目录用于其传递依赖搜索。Linux 入口必须为原生 x86-64 ELF；宿主解析 ELF 元数据收集依赖，不执行 `ldd` 或包内安装脚本。包内 ELF 文件 basename 必须唯一，私有库目录形成固定 `LD_LIBRARY_PATH`；依赖优先由包内文件满足，其余仅复制支持的、root 所有且不可由普通用户写入的系统运行库。不挂载宿主 `/usr`，不继承宿主 PATH、LD_* 或凭据环境。

脚本可作为载荷，但入口必须是明确打包的兼容原生解释器，脚本通过包内路径参数传入；不会自动调用宿主 Python、Node、shell 或下载运行时。包内所有文件仍受 128 文件、64 MiB 解码载荷限制。大运行时、重复 ELF 名称和未声明依赖可能被拒绝，安装成功不保证任意语言运行时均兼容。

不导入或回写宿主工作区，不提供网络放行字段。传入宿主绝对路径不会自动授予读取/写入权限。Windows 的系统资源仍由 LPAC ACL 决定，Linux 保留最小设备和 `/proc`；这不等同于虚拟机。可用的宿主文件工具仍按独立授权工作。

## 生命周期与续跑

每次连接先验证所有已授权插件，再在逐个启动前重验。导入副本按冻结文件清单检查，避免先核验原文件、随后复制变更字节仍执行。准备阶段接受取消；同步 OS 调用不保证即时响应。启动与 RPC 超时、取消、连接销毁会终止所属 Job/cgroup 并清理私有目录。完整包大小有应用层上限，scratch 没有 OS 磁盘配额。

进程树生命周期、限制和工具调用回执沿用现有 Harness。派发后结果未知会阻止重放；重启不会恢复插件内存或 scratch。权限及 `isolated-process-package` 执行语义与锁一起进入任务绑定：修改上限、沙箱模式、包或锁均不能静默续跑。旧单文件沙箱插件任务没有这份完整包授权，也会拒绝续跑；不转换历史绑定。

沙箱只读约束针对被隔离的进程，不承诺抵抗宿主上具有同用户、root 或管理员权限的攻击者。系统库不是插件版本锁的一部分。硬杀宿主或 OS 清理失败可能留下暂存资源，程序不猜测删除其他操作的目录；不自动降低权限或转为 `none`。

`doctor.plugin_sandbox` 和 `initialize.capabilities.plugin_sandbox` 描述编译支持，当前机器的真实验证结果见[验证记录](validation.md)。
