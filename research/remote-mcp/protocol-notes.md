# 远程 MCP Streamable HTTP 协议核对

核对日期：2026-10-25。目标规范：MCP `2025-11-25`；本文不声称它是当前最新发布版本。

本文用于 CodeStudio Agent 的首版远程 MCP 实现与验收，区分规范要求、可选能力与项目策略。它是协议研究记录，不是实现完成或测试通过的凭据。

## 范围与现有约束

实现范围为单端点 Streamable HTTP、JSON/SSE 响应、会话与断线恢复，以及显式配置的 Bearer 凭据引用。OAuth 发现、登录、令牌获取与刷新不在本轮范围，因此不能将该实现描述为完整实现 MCP Authorization 规范。历史 HTTP+SSE 双端点传输也不在本轮范围。[T][A]

已有 `src/mcp/mcp.cpp` 在本次核对时请求 `2025-11-25`，接受 `2025-11-25`、`2025-06-18`、`2025-03-26`。是否继续支持这些版本应由共用握手逻辑显式决定；不能因增加 HTTP 而绕过版本与能力校验。外部日期协议版本与本地 MCP 清单、任务协议、数据库版本各自独立。[L]

远程传输继续服从现有工具白名单、参数与结果校验、消息/schema 预算、执行回执及结果不明时不自动重放的规则。模型提供商凭据不自动用于 MCP；插件服务收到配置许可不等于获得额外宿主权限。这些是项目约束，不是传输规范授予的能力。

## HTTP 与生命周期要求

| 项目 | 规范要求 | 实现与验收含义 |
| --- | --- | --- |
| 发送消息 | 每个客户端 JSON-RPC 消息 MUST 使用新 POST；正文 MUST 是一个请求、通知或响应，JSON-RPC 使用 UTF-8。[T] | 不发送批量数组；通知与对服务器请求的响应也使用独立 POST。 |
| 内容协商 | POST 的 `Accept` MUST 同时包含 `application/json` 与 `text/event-stream`；请求响应 MUST 支持 JSON 对象和 SSE 两种形式。[T] | 按响应媒体类型选择解析器；不要把每个 HTTP 成功响应都当作 JSON。 |
| 无响应消息 | 被接受的通知与 JSON-RPC 响应，服务器 MUST 返回 `202` 且无正文。[T] | `notifications/initialized`、`notifications/cancelled`、ping 响应各自检查接收状态。 |
| 初始化 | `initialize` MUST 是首次交互；成功后客户端 MUST 发送 `notifications/initialized`。客户端 SHOULD NOT 在初始化结果前发送 ping 以外的请求。[L] | 校验协议、必需能力及服务器信息后才发现工具。 |
| 协议头 | 初始化后所有 HTTP 请求 MUST 携带 `MCP-Protocol-Version`；该头 SHOULD 使用协商得到的版本，运行过程 MUST 尊重协商版本。[T][L] | POST、恢复 GET、DELETE 共用协商状态；不固定发送请求时原始版本。 |
| 会话分配 | 服务器 MAY 在初始化响应中分配 `MCP-Session-Id`；其字符 MUST 为可见 ASCII `0x21–0x7E`。[T] | 校验并安全保存会话 ID；不将其作为认证令牌。 |
| 会话传递 | 若获得会话 ID，客户端 MUST 在随后所有 HTTP 请求中携带它。[T] | 包括通知、服务器请求的响应、恢复 GET 和终止 DELETE。 |
| 会话失效 | 带会话 ID 的请求收到 `404` 时，客户端 MUST 发送不带旧会话 ID 的新 `initialize`，开始新会话。[T] | 新会话不能被解释成原工具调用未执行；重新初始化与重发工具调用是两件事。 |
| 会话终止 | 不再需要会话时客户端 SHOULD 发 DELETE；服务器 MAY 返回 `405` 表示不支持客户端终止。[T] | 清理有独立且有限的时间预算；终止失败不能覆盖原始操作结果。 |
| 独立监听 | 客户端 MAY 打开独立 GET SSE 流；GET 的 `Accept` MUST 包含 `text/event-stream`，服务器返回 SSE 或 `405`。[T] | 首版可不提供常驻独立监听，但这不等于可以省略已发请求的恢复 GET。 |

SSE 内允许先出现与原请求相关的服务器请求和通知，再出现对应 JSON-RPC 响应。保留现有 ping 处理；未声明支持的客户端能力不能因换用 HTTP 而自动开放，对不支持的请求返回对应 JSON-RPC 错误。服务器请求的响应仍经单独 POST 发送。[T][L]

## SSE 与恢复

### 消息流恢复

1. 服务器 SHOULD 立即发送包含事件 ID 与空 `data` 字段的 SSE 事件，以便客户端获得恢复游标。它是有效的传输控制事件，不能作为空 JSON 消息解析失败。[T]
2. 服务器发送过带 ID 的事件后 MAY 关闭物理连接，同时保持逻辑流未结束；客户端 SHOULD 尝试重新连接。服务器 SHOULD 在这类关闭前发送 `retry`，客户端 MUST 按该值等待相应毫秒数。[T]
3. 恢复使用同一个 MCP 端点的 **GET + `Last-Event-ID`**，即使原流由 POST 发起。不得通过重新发送原 `tools/call` POST 来恢复响应。[T]
4. 游标对应原逻辑流。事件 ID 在会话所有流中 MUST 全局唯一；无会话时在与该客户端的所有流中唯一。服务器 MUST NOT 用一个流的游标重放另一个流的消息。客户端应把游标保存在各自请求/流状态中。[T]
5. 没有恢复游标的独立 GET 流不能接收 JSON-RPC 响应；恢复 GET 则可以接收原请求的响应。不要把未知请求 ID 的响应当作当前调用结果。[T]
6. 服务器 MAY 提供可恢复流，协议没有保证任意断线都能恢复。没有可用游标、恢复返回 `405`、会话失效、消息错误或耗尽请求预算时，应结束等待并按项目规则保留结果不明状态，不能从“没收到结果”推断“没有副作用”。[T]

恢复重试、通知处理和服务端 `retry` 必须受原请求的总截止时间约束。`retry` 长于剩余预算时可以超时结束，但不能为了更快重连而提前违反等待时间；限制重连次数属于项目资源策略，应明确记录。[T][L]

### SSE 解析细节

以下以 WHATWG SSE 算法为依据，而不是以常见的 `split("\n\n")` 简化格式为依据。[W]

- UTF-8 解码并剥离一个开头 BOM；处理 LF、CRLF、单独 CR，以及网络 chunk 恰好拆开 CRLF/BOM/UTF-8 字节的情况。
- 字段名区分大小写；按第一个冒号分割，值开头只去掉一个空格。无冒号的行视作字段名和空值；注释与未知字段忽略。
- 多个 `data` 行用 LF 拼接，派发前去掉最后追加的 LF；没有显式事件类型时默认为 `message`。
- 遇到空行才派发事件。EOF 不补派发未结束的事件；未完成的数据丢弃。
- `id` 值含 NUL 时忽略该字段；空 `id` 清空游标。事件派发步骤先更新最后事件 ID，再判断是否有数据。只有 ID、没有 `data` 的完整事件块也能更新游标。
- `data:` 空字段会形成空字符串消息事件；MCP 的空 primer 应更新游标，不进入 JSON-RPC 解码。
- `retry` 只接受 ASCII 十进制数字，其他值忽略；整型溢出不能变成较短等待或忙循环。数值处理限制属于客户端策略。
- SSE 事件 ID 是不包含 NUL/LF/CR 的 UTF-8 字符串，不能套用 MCP session ID 的可见 ASCII 限制。若实现额外字节长度/字符限制，必须作为客户端限制说明，而非引用为 MCP 规范要求。

Windows 实现注意：`WinHttpAddRequestHeaders` 的官方文档说明了头名称/值校验，但没有在该页面保证 UTF-8 原始字节的映射行为。另一个 API `HttpAddRequestHeadersW` 属于 WinINet，其 ISO-8859-1/UTF-16LE 说明不能直接当作 WinHTTP 保证。若首版不能可靠发送非 ASCII `Last-Event-ID`，应明确拒绝该恢复操作并注明平台限制，不能改写游标、百分号转义或重新 POST 工具请求；Linux 与 Windows 的该项验收结果应分别记录。[WH][WI]

无显式 `id` 的事件会继承先前事件 ID。如果为了避免重复处理服务器请求，在已经交付这类业务消息后保守地停止自动恢复，这属于客户端策略；应说明其可用性代价，且不要让注释、空 primer 等没有业务消息的事件无故禁用恢复。[W]

## 取消、超时与执行回执

`notifications/cancelled` 的 `params` 包含 `requestId` 和可选 `reason`，只可引用本方向已经发出且认为仍在执行的请求。客户端 **MUST NOT 取消 `initialize`**。普通请求超时后 SHOULD 通知取消并停止等待；取消可因请求未知、完成或无法取消而被对端忽略。连接中断不是取消，取消也不能证明回滚。[T][L][C]

本轮不增加 MCP task-augmented 请求。它们要求使用 `tasks/cancel`，不能借用普通 `notifications/cancelled`。[C]

项目策略应明确区分：

- 原 POST 尚未发出：可以按已有调度与授权规则决定是否继续。
- 原 POST 已发出但响应连接断开：只能尝试恢复原响应流；继续保持同一执行身份，不产生第二次工具调用。
- 原 POST 已发出后发生 `401`、`403`、`404`、超时、取消或无法确认的结果：这些信息不构成副作用未发生的证明，执行账本保持保守语义。
- 带会话 ID 收到 `404`：按协议开始新会话，同时结束旧调用的自动推进；不能在新会话中偷偷重放旧工具调用。若新会话工具/schema 发生变化，重新经过发现与授权边界，不能沿用未经检查的新定义。
- `202` 只说明服务器接受某个通知或响应，不证明工具完成或取消生效。

以上账本与不重放策略是项目选择；MCP 规范的恢复能力本身不提供工具调用的 exactly-once 执行保证。

## 凭据与网络安全边界

使用认证时，`Authorization: Bearer <token>` MUST 随每个 HTTP 请求发送，包括同一会话中的后续请求；令牌 MUST NOT 放在 URI 查询参数中。令牌应明确用于目标 MCP 服务。认证失败/过期常见为 `401`，权限或 scope 不足为 `403`。[A]

首版采用显式凭据引用，凭据解析只进入 HTTP 头；不把令牌写入 manifest、URL、诊断、数据库回执或模型上下文。模型提供商的 API key 不自动复用。遇到 `401`/`403` 清晰失败，不通过自动获取凭据后重放工具调用来恢复。这些是本项目的凭据与执行策略，不代表完整 OAuth 实现。

官方安全建议涉及 OAuth 发现 URL 的 HTTPS、私有/保留地址限制与重定向目标验证；不能把针对发现链路的建议不加区分地称为“所有显式局域网 MCP 地址都被协议禁止”。本轮显式端点配置、禁用自动重定向、生产环境 HTTPS，以及明确受控的本地 HTTP 测试例外，可减少凭据被发往服务器选择的新地址的风险。[S]

服务器对 `Origin` 的检查是服务器职责；客户端实现不能声称替服务器完成该防护。Session ID 也不能替代身份认证。[T][S]

## 建议验收矩阵（待实现测试）

| 类别 | 场景与需要观察的事实 |
| --- | --- |
| 基本往返 | initialize → initialized → tools/list → tools/call；JSON 与 SSE 两种响应；通知/响应的独立 POST 返回空 `202`。 |
| 会话与版本 | 分配/不分配 session；协商版本头与 session 出现在所有后续方法；非法 session 字符拒绝；清理 DELETE 的成功与 `405`。 |
| SSE 分帧 | 按单字节/chunk 拆分 BOM、UTF-8、CRLF；LF/CR；多行 data；注释、未知字段、大小写、无冒号字段；默认 event；未终止事件在 EOF 不派发。 |
| 游标规则 | 空 data primer、仅 id 的事件块、空 id 清除、含 NUL 的 id 忽略、UTF-8 id、事件间继承 id；响应 ID 与原请求一致。 |
| 恢复 | 原 POST 计数严格为 1；断线后 GET 携带原流 `Last-Event-ID`；恢复响应成功；retry 实际等待；多次断线仍受总截止时间约束。 |
| 恢复失败 | 无游标、GET `405`、会话 `404`、未知响应 ID、非法媒体类型、消息/通知预算超限；无隐式 POST 重发。 |
| 会话过期 | 含 session 的请求/恢复 GET 收到 `404` 后，新 initialize 不带旧 session；旧 tools/call 未在新会话重放；新 schema 重新校验。 |
| 双向 RPC | SSE 先发送 ping/其他主动请求/通知再返回结果；响应通过新 POST；不支持的客户端能力被拒绝。 |
| 取消与超时 | 活跃请求发送取消；初始化超时不发送取消；取消被忽略/结果晚到的竞态；关闭连接不记作确定已取消；进度不能无限延长总期限。 |
| 凭据 | initialize/POST/恢复 GET/DELETE 使用显式 MCP 凭据；缺失凭据、`401`、`403`；没有模型 key 回退；日志与账本无明文 token；重定向不外发凭据。 |
| 回归与平台 | stdio 原有行为、工具 grants、schema/message 预算与 unknown 账本规则不回退；Windows/Linux 由主实现流程统一构建与运行测试。 |

## 一手来源

- [T] [MCP 2025-11-25 Transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports.md)：HTTP 消息、响应类型、会话、协议头、SSE 与恢复。
- [L] [MCP 2025-11-25 Lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle.md)：初始化、版本/能力协商与超时。
- [C] [MCP 2025-11-25 Cancellation](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/cancellation.md)：取消约束及竞态。
- [A] [MCP 2025-11-25 Authorization](https://modelcontextprotocol.io/specification/2025-11-25/basic/authorization.md)：Bearer 使用、授权错误与 OAuth 要求。
- [S] [MCP 2025-11-25 Security Best Practices](https://modelcontextprotocol.io/specification/2025-11-25/basic/security_best_practices.md)：SSRF、重定向、凭据用途与会话安全。
- [W] [WHATWG Server-sent events](https://html.spec.whatwg.org/multipage/server-sent-events.html)，特别是 [Parsing an event stream](https://html.spec.whatwg.org/multipage/server-sent-events.html#parsing-an-event-stream)、[Interpreting an event stream](https://html.spec.whatwg.org/multipage/server-sent-events.html#event-stream-interpretation)、[The Last-Event-ID header](https://html.spec.whatwg.org/multipage/server-sent-events.html#the-last-event-id-header)：UTF-8、字段解析、派发、EOF 和重连游标。
- [WH] [Microsoft WinHttpAddRequestHeaders](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpaddrequestheaders)：WinHTTP 请求头接口和格式校验。
- [WI] [Microsoft HttpAddRequestHeadersW](https://learn.microsoft.com/en-us/windows/win32/api/wininet/nf-wininet-httpaddrequestheadersw)：不同的 WinINet API，明确描述头值的 ISO-8859-1/UTF-16LE 映射；不能据此推断 WinHTTP 保证。
