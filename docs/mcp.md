# MCP 工具扩展

CodeStudio-Agent 通过 MCP stdio 加载独立进程插件，也可通过 Streamable HTTP 连接显式授权的远程服务。两种传输共用工具授权、模型循环和 SQLite 动作回执。

## 配置与运行

参考 [示例清单](../examples/mcp.example.json)，将 executable 和脚本路径替换为本机的绝对路径。本地 MCP 配置只接受 `version: 2`；其他版本会在服务器启动前拒绝，不进行格式转换。该版本号描述 CodeStudio-Agent 的本地配置格式，与 MCP 握手中的日期协议版本无关。`command` 必须指向已存在的可执行文件，`args` 是独立参数数组，不经过隐式 shell。`tools` 是允许暴露的工具名称列表，不支持自动授权全部工具。

```json
{
  "version": 2,
  "servers": [{
    "id": "echo",
    "command": "C:/Path/To/python.exe",
    "args": ["D:/Path/To/CodeStudio-Agent/examples/mcp_echo.py"],
    "tools": ["echo"],
    "timeout_ms": 10000
  }]
}
```

检查工具：

```powershell
xmake run codestudio-agent mcp-tools --workspace . `
    --mcp-config .\mcp.json --allow-mcp echo
```

接入已配置的模型：

```powershell
xmake run codestudio-agent run --workspace . `
    --mcp-config .\mcp.json --allow-mcp echo `
    --prompt '调用 mcp_echo_echo，返回 Hello from MCP。'
```

每个服务器都必须通过 `--allow-mcp <id>` 显式启用。读取 manifest 不代表启动许可；不会扫描工作区并自动启动插件。模型看到的工具名为 `mcp_<server-id>_<tool-name>`，避免覆盖内置工具。服务器 ID 最长 16 字符，暴露后的名称最长 64 字符，仅接受 ASCII 字母、数字、下划线和连字符。

`mcp-tools` 会启动所授权的 stdio 服务器或连接所授权的 HTTP 端点，进行握手与工具发现，然后关闭；它不是纯静态配置查看命令。stdio 许可覆盖进程启动及清单允许的工具。默认 `--sandbox none` 使进程插件拥有当前用户权限，配置的脚本、依赖和 executable 都必须来自可信来源。

## 远程 Streamable HTTP

参考 [远程示例清单](../examples/mcp-http.example.json)。`transport: "streamable-http"` 使用单个端点，支持 JSON 与 SSE 响应；省略 `transport` 或填写 `stdio` 使用上述进程传输。

```json
{
  "version": 2,
  "servers": [{
    "id": "remote",
    "transport": "streamable-http",
    "url": "https://tools.example.com/mcp",
    "credential": "system:remote-tools",
    "tools": ["echo"],
    "timeout_ms": 10000
  }]
}
```

`credential` 是可选的 `env:VARIABLE` 或 `system:NAME` 引用，使用已有的[凭据管理](credentials.md)。未指定则匿名连接，不借用模型密钥。令牌仅发往冻结配置中的端点，不跟随重定向、不启用 HTTP 自动登录。每次运行或显式续跑重新解析凭据，在同一次操作内固定；后台 worker 仍受自身环境及系统凭据库可用性限制。

先配置凭据，再检查工具：

```powershell
codestudio-agent mcp-tools --workspace . --mcp-config .\remote-mcp.json --allow-mcp remote
```

`run`、`resume` 和任务服务的 `mcp_config` / `allow_mcp` 使用相同配置。配置原文、路径和 grants 纳入授权绑定，续跑不能替换地址或凭据引用；同一引用下的凭据值可以轮换。所有配置项、grant 和已授权凭据先完整检查，随后才启动进程或发出网络请求。配置冻结后不会再次读取文件来选择令牌的发送地址。拟暴露给模型的工具 schema 含已解析凭据时拒绝加载，工具结果和流式输出沿用统一脱敏。

端点默认要求 HTTPS 且验证证书。仅清单中的 `allow_http_loopback: true` 允许 `http://127.0.0.1` 或 `http://[::1]`，HTTP 例外不接受 `localhost`、其他 HTTP 地址、URL userinfo、查询串或 fragment。该选项与模型接口的命令行开关独立。HTTP 条目不能包含 `command` / `args`，stdio 条目不能包含 HTTP 字段。远程服务无法兑现本地进程沙箱授权，因此授权 HTTP 与 `--sandbox appcontainer` / `linux-namespace` 的组合在连接前拒绝。

HTTP 会话 ID 限定为 1..1024 字节可见 ASCII，与协商后的协议版本随后续请求发送；正常关闭时在 250 ms 预算内尝试 DELETE，会话拒绝 DELETE 不使已完成调用变成失败。会话/token 不写入检查点。会话过期 `404` 使当前连接失效；新的显式加载重新 initialize 和发现工具，旧调用仍然保留原回执。

SSE 在已有完整事件游标时，最多通过两次 GET + `Last-Event-ID` 恢复原响应流；遵守服务端 `retry` 和原请求总期限，绝不重发原 POST。相同事件 ID 的相同数据不会再次派发；ID 对应数据改变则失败。无游标、已交付无 ID 的业务事件、HTTP 错误、协议错误或预算耗尽均可能终止恢复。Windows 首版仅支持 ASCII SSE 事件 ID；Linux 支持 UTF-8。事件 ID 最多 1024 字节，不接受首尾空格，控制字符不接受（含 NUL 的 id 字段按 SSE 规则忽略）。

超时或取消时尝试发送 `notifications/cancelled`（另设 250 ms 清理预算），初始化请求不发送取消。通知不是撤销保证：已经派发但无法确认结果的工具仍记为 `unknown`，阻止自动续跑和显式重放。实现不创建长期独立 GET 订阅，只恢复当前请求的 SSE 响应。

对于 stdio，Windows 的 `mcp-tools`、`run` 和 `resume` 可使用 `--sandbox appcontainer`。同一运行的全部 MCP 子进程进入 LPAC：只暂存清单中的一个 executable，不复制脚本、私有 DLL 或配置，不授权原工作区，且不授予网络 capabilities。上述 Python 脚本例子不能直接套用该模式；应先准备适合通过 stdio 处理输入的自包含服务器。依赖不可用时失败，不会自动以 `none` 重试。详见 [沙箱边界](sandbox.md)。

每个启用 OS 沙箱的 stdio 连接有自己的可写 scratch；一次连接内可使用它保存临时数据，关闭或重新连接后不保留。`none` 模式的 stdio 进程直接使用配置工作区。`resume` 重新启动 stdio MCP，既不恢复插件内存，也不恢复沙箱 scratch。HTTP 服务的存储由服务端管理。工具应对状态缺失作出明确响应。沙箱临时输出不会自动导入工作区。

## 执行与失败语义

加载时进行 `initialize` / `notifications/initialized`、协议版本协商和有上限的 `tools/list` 分页。工具 schema 在本次运行内固定。调用先经过 host 参数检查，再记录 prepared/dispatched 意图，然后发送 `tools/call`，最后保存结果。

server annotations 不能授予权限或证明“只读”。MCP 成功回执记录为 `applied`，只证明服务器报告成功；服务器报错、失联、超时或返回无法确认的内容时保守记录 `unknown`，停止自动推进。不会自动重发一次结果不明的操作。运行中工具列表变化会使当前连接失效，重新启动任务才能重新发现。

RPC 有消息大小、超时、通知数量和发现数量限制：每条消息最多 1 MiB，单次交换最多 4 MiB，单个服务器最多发现 512 项/16 页，每次请求最多处理 128 条旁路消息；HTTP SSE 另限制最多 128 个事件及 64 KiB 响应头。所有已授权服务器合计最多暴露 128 个工具，暴露的 schema 与保留的 outputSchema 共用 1 MiB 预算。stdio 的 stdout 仅承载换行分隔的 JSON-RPC，stderr 单独排空；日志污染 stdout 会导致协议失败。进程树在退出、取消或失败时回收。

## 当前边界

支持 stdio 和 Streamable HTTP 的工具发现和调用，协议版本为 2025-11-25、2025-06-18、2025-03-26；OAuth 发现/登录/刷新见[OAuth 连接](mcp-oauth.md)，精确版本锁定与分发见[插件包](plugins.md)。暂不支持旧 HTTP+SSE 双端点传输、resources/prompts、sampling、elicitation、MCP tasks 和插件市场。未声明的服务器主动请求会被拒绝，ping 可响应。工具回执中的媒体和资源仅作为数据保留，不自动访问 URI。HTTP 可使用显式 Bearer 引用或公开客户端 OAuth；当前 OAuth 的注册与兼容范围见专门文档。

只进行常见 JSON Schema 约束检查，包括对象字段、类型、枚举、常量和大小/数值边界；已声明的 outputSchema 也会检查结构化结果。`$ref`、组合条件和正则等未完整实现，完整业务输入校验仍由 MCP 服务器承担。配置文件不是密钥存储，不支持在其中注入环境变量。子进程使用受限的环境变量集合；`none` 模式仍可凭当前用户权限读取磁盘。Job Object 负责生命周期，文件/网络隔离由明确选择的 LPAC 后端提供。LPAC 依旧受系统 ACL 控制，并无资源配额或完整第三方插件安全认证。

协议依据：[Lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)、[Transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)、[Tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)（stdio 部分于 2026-10-21 核对；HTTP 补充依据见 [2026-10-25 协议核对](../research/remote-mcp/protocol-notes.md)）。

启用 `--build-config` 时，构建工具和 MCP 共用 128 个扩展工具及 1 MiB schema 预算。加载器在启动服务器前预留构建工具的数量/字节；实际远程 schema 的字节超限在发现阶段拒绝并回收连接。
