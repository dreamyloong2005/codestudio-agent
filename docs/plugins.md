# 插件版本锁定与分发

托管插件是带精确版本、平台和文件摘要的 MCP stdio 程序包。插件包、安装内容、锁文件和运行授权各自承担不同作用：包携带代码，安装保存经过核验的内容，锁文件选择一个确定版本，MCP 清单及 `--allow-mcp` 决定是否启动及暴露哪些工具。

包不包含安装钩子，也不改变 PATH、系统服务、凭据或工具授权。当前不实现在线插件市场、原生动态库 ABI 插件、版本范围解析或自动更新。

## 制作确定的插件包

在独立源目录中准备程序及依赖，并创建 `plugin.json`：

```json
{
  "format": 2,
  "id": "example-echo",
  "version": "1.2.3",
  "platform": "windows",
  "arch": "x64",
  "entrypoint": "echo-server.exe",
  "args": ["--config", "${plugin}/config.json"],
  "tools": ["echo"]
}
```

Linux 使用 `platform: "linux"`、`arch: "x86_64"`，并指定对应的程序路径。一个包只对应一个原生平台；打包器拒绝与自身平台不符的描述。版本为无前导零的 `major.minor.patch`，不接受范围、`latest`、预发布或构建后缀。插件 ID 仅允许小写 ASCII 字母、数字、下划线、连字符。

`entrypoint` 必须是源目录内的相对文件；Linux 安装后具有执行权限。`args` 是独立参数数组，禁止隐式 shell。`${plugin}/相对路径` 展开到安装中的已声明文件或目录；不接受其他变量模板。普通字符串按字面传入，包括作者明确指定的绝对参数路径；这不自动授予文件访问权限。

```text
codestudio-agent plugin-pack --source <源目录> --output <目录外的包.csplugin>
```

源描述不填写 `files`。打包器枚举全部载荷，计算每个文件的 SHA256、字节数和执行属性，生成格式 2 的清单和 Base64 载荷。归档为排序、紧凑 UTF-8 JSON 加一个末尾换行；相同源内容和描述生成相同字节。已存在的相同包输出可重复使用；不同内容的输出拒绝覆盖，应选择新文件名。

包最多 128 个载荷文件、64 MiB 解码后总量、96 MiB 归档大小。路径必须使用正斜线和 ASCII 文件名，拒绝遍历、设备名、大小写别名、符号链接、硬链接、Windows reparse point、特权权限和非普通文件；空目录也不进入包。`plugin.json`、`bundle.csplugin` 为保留的顶层名称。源目录不能同时作为归档输出目录。

## 安装与分发

将 `.csplugin` 文件及通过可信渠道取得的完整 SHA256 交给使用者：

```text
codestudio-agent plugin-install --bundle <包.csplugin> --sha256 <64位小写SHA256> --store <插件存储目录>
codestudio-agent plugin-install --url https://downloads.example.com/echo.csplugin --sha256 <64位小写SHA256> --store <插件存储目录>
```

两种安装方式都强制要求预先提供的 SHA256，验证完整归档后才创建安装目录。HTTPS 下载验证证书，最多等待 30 秒，不跟随重定向，不使用环境代理、HTTP 自动登录、Cookie 或模型密钥。URL 不允许 userinfo、查询串或 fragment；私有下载服务应提供可直接访问的明确 HTTPS 地址。仅显式 `--allow-http-loopback` 允许 `http://127.0.0.1` 或 `http://[::1]`，主要用于本机夹具，不接受 `localhost` 或其他明文 HTTP 地址。

安装目录固定为：

```text
<store>/<id>/<version>/<bundle-sha256>/
```

其中保存派生的 `plugin.json`、原始 `bundle.csplugin` 和载荷。相同内容重复安装先核验现有全部文件，成功时返回 `unchanged`。相同版本的新摘要和新版本分别安装在独立目录，不覆盖旧版本或更改旧锁文件。已有目录包含额外、缺失、改动或链接文件时拒绝接管。

SHA256 证明内容与事先选定的摘要一致，不能单独证明发布者身份。当前没有签名验证、信任根、远程注册表或自动发布能力。运行插件前仍需信任其来源。

## 锁定并接入 MCP

使用安装返回的 `directory` 生成锁文件，再检查它：

```text
codestudio-agent plugin-lock --directory <已安装的摘要目录> --output <插件.lock.json>
codestudio-agent plugin-inspect --lock <插件.lock.json>
```

锁文件格式 2 固定记录 `id`、精确 `version`、`platform`、`arch`、`bundle_sha256`、`manifest_sha256` 和绝对安装目录。锁不能放进不可变的安装目录。再次对同一输出执行 `plugin-lock` 是显式重新选择版本，采用暂存与原子替换；不兼容或非锁文件输出拒绝覆盖。`plugin-inspect` 核验锁与文件后返回身份、展开后的命令参数和工具声明，不执行插件。

在 MCP 清单中，用 `plugin_lock` 替代 `command` 和 `args`：

```json
{
  "version": 2,
  "servers": [{
    "id": "echo",
    "plugin_lock": "D:/Plugins/echo.lock.json",
    "tools": ["echo"],
    "timeout_ms": 10000
  }]
}
```

```text
codestudio-agent mcp-tools --workspace <工作区> --mcp-config <MCP清单> --allow-mcp echo
```

`plugin_lock` 是绝对路径，不能混用可变的 `command` / `args` 或 HTTP 传输字段。清单中的 `tools` 必须是插件声明工具的子集，仍需逐个明确配置；包安装和锁定都不会自动创建 `--allow-mcp` 授权。

运行前一次性读取锁文件，核验原始包、安装清单、完整载荷集合、摘要及 Linux 执行属性。启动前再次核验同一冻结身份；不在同一操作中重新读取锁来选择不同版本。锁的原文进入任务授权绑定，因此更换版本、安装位置或锁内容后，旧任务不会静默续跑到新代码；需要恢复原锁和内容，或创建明确的新任务。

锁只覆盖包内代码及文件，不冻结系统动态库、外部绝对参数路径、MCP 服务端内存或远程资源。摘要目录是不可覆盖的管理约定，并非 OS 只读挂载；同用户进程仍可能修改文件，核验会拒绝已观察到的变化，但检查到启动之间不构成抵抗恶意同用户并发替换的完整保证。可信来源、独立用户和实际 OS 沙箱仍然决定执行隔离。

锁定插件配合 `--sandbox appcontainer` 或 `--sandbox linux-namespace` 时，整个包的程序、私有库和配置会暂存到只读隔离副本，`${plugin}/...` 自动指向该副本。进程/内存/CPU 上限由宿主 MCP 清单的 `sandbox_limits` 明确指定；工作区及网络仍不可访问。用法、解释器与动态库限制见[完整插件包隔离](plugin-sandbox.md)。未锁定的普通 `command` 仍使用原单程序沙箱，沙箱失败不自动降级。

## 故障与运维边界

安装以唯一暂存目录完成所有文件写入和验证后发布。存储内同版本安装操作和锁文件写入使用独占操作锁；不同进程不会以静默覆盖处理冲突。中断或磁盘故障可能留下 `.plugin-stage-*`、临时输出或操作锁，保留供人工检查；不扫描或猜测删除既有文件。工具未提供强制删除、垃圾回收或插件卸载命令。

本地打包、校验和发布是有大小上限的同步操作。URL 下载可取消；发布前观察到取消会拒绝开始安装，已完成发布不会因随后取消而回滚。任务服务的取消也不保证撤销已完成的文件操作。限制是应用层计数，不能替代 OS 磁盘配额，也不声明断电事务保证。

程序升级应先安装到新内容目录，检查锁的目标，再显式更新锁。不要在运行的插件目录中原位修改程序，也不要把会产生可写状态的工作区设为插件安装目录。插件任务状态仍保存在调用者工作区，远程模型和 MCP 工具的未知效果仍禁止自动重放。
