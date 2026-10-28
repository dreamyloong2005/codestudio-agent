# MCP OAuth 连接

OAuth 连接与 MCP 工具授权分别管理。登录只取得访问一个 MCP 资源的令牌；运行或发现工具仍需明确的 `--allow-mcp`。实现使用预注册的公开客户端、Authorization Code + PKCE S256、临时 IPv4 回环回调、固定 issuer/resource/client/scopes 和系统凭据存储。

```json
{
  "version": 2,
  "servers": [{
    "id": "remote",
    "transport": "streamable-http",
    "url": "https://tools.example.com/mcp",
    "oauth": {
      "issuer": "https://auth.example.com",
      "client_id": "your-public-client-id",
      "credential": "system:remote-oauth",
      "scopes": ["tools.read"]
    },
    "tools": ["echo"],
    "timeout_ms": 10000
  }]
}
```

`oauth` 与服务器级 `credential` 互斥，不能用于 stdio。必须先在授权服务器配置公开客户端及 `http://127.0.0.1:<临时端口>/oauth/callback` 的原生应用回调规则。当前不实现动态客户端注册、客户端密钥、Device Code 或自动扩大 scopes。固定 issuer 必须出现在受保护资源元数据中，授权服务器元数据必须返回相同 issuer 并声明 S256。

```text
codestudio-agent mcp-auth-discover --mcp-config remote.json --server remote
codestudio-agent mcp-auth-login --mcp-config remote.json --server remote --login-timeout-ms 300000
codestudio-agent mcp-auth-status --mcp-config remote.json --server remote
codestudio-agent mcp-tools --workspace . --mcp-config remote.json --allow-mcp remote
codestudio-agent mcp-auth-logout --mcp-config remote.json --server remote
```

`discover` 访问元数据，不启动工具。`login` 在 stderr 输出一条带 `authorization_url` 与 `redirect_uri` 的 JSON 通知，然后等待用户在浏览器完成授权；stdout 仅输出最终 JSON 结果。程序不自动打开浏览器。登录等待预算为 100..300000 ms，元数据/令牌请求另受清单 `timeout_ms` 限制。取消、超时、state/issuer 不匹配、无效响应和缺失系统库均不回退到匿名或明文存储。

访问令牌、刷新令牌及其绑定共同保存在独立的当前用户 OAuth 存储域中：Windows Credential Manager 的 `CodeStudio-Agent/oauth/`，Linux Secret Service 的独立内部标识，以及 macOS Keychain 的独立 OAuth service。普通 `credential-*` 命令或模型的 `system:` 引用不能读取这个域；OAuth 的 `system:NAME` 只选择 OAuth 记录名。记录总长度最多 2048 字节；超过限制的服务令牌组合会明确拒绝，不能写入清单、日志、数据库、检查点或 shell 参数。Linux 需要已可用且解锁的 Secret Service；macOS 需要当前默认 Keychain 已可读取。程序不提示解锁、不创建用户密钥环。

每次新运行、显式续跑或调度执行前重新读取令牌。即将过期时先在跨进程锁中刷新，再固定本次操作的令牌。刷新派发前将记录标记为不可重试；如果请求、持久化或进程发生不确定失败，后续运行拒绝复用旧刷新令牌，需要明确重新登录。运行中收到 401 不自动刷新、重新初始化或重发工具调用。已经派发且无法确认的工具继续按未知效果处理。

刷新只在完整参数、权限及续跑绑定检查通过后进行；仅把已有任务加入续跑队列不会刷新或连接 MCP。多个连接先统一检查本地授权状态，再依次刷新。新旧令牌均参与输出、检查点、cron 元数据及构建提案保护；包含已知令牌的授权通知会整体拒绝，不输出损坏或泄密的链接。

`status` 和 `logout` 只访问本地系统库。状态区分 `available`、`expired`、`refresh_blocked`、`missing`、`binding_mismatch`、`locked`、`unavailable`。`logout` 仅删除本地记录，不代表远端撤销；需要撤销服务器授权时应使用服务器的账号管理功能。旧任务绑定中的 issuer、resource、client_id、scopes、凭据引用仍需精确一致，同一连接下的令牌轮换不改变任务权限。

同名 OAuth 记录绑定另一份配置时，登录和删除均拒绝；先恢复原配置并退出登录，再使用新配置，或选择新的记录名。重新登录只在新令牌响应通过检查并安全保存后替换旧记录；失败的替换可能保留旧记录，但不会自动重发已交换的授权码。授权服务器是否已撤销旧令牌仍由服务器决定。

当前实现要求授权、令牌和元数据地址不含查询串；`resource_metadata` 必须与配置的 MCP 资源同源。网络请求验证 HTTPS 证书，不跟随重定向，不使用代理、系统自动登录或 Cookie；测试用明文仅接受明确启用的字面回环地址。令牌响应必须包含正数且有上限的 `expires_in`，返回的 scope 集合须与显式配置一致。令牌剩余有效期不超过 30 秒时需要刷新；没有刷新令牌则要求重新登录。

登录、刷新和退出登录按凭据名串行。Windows 使用当前用户身份限定的命名互斥量；Linux 使用当前网络命名空间内的 UID/凭据名限定抽象 Unix socket，退出时由内核释放，不遗留锁文件。不要让不同 Linux 网络命名空间中的进程同时刷新同一系统 OAuth 记录；锁协调不代替系统凭据库的访问控制。macOS 使用当前用户私有目录中按凭据名散列的 `flock` 锁；核对目录/文件归属、权限及身份，拒绝链接，进程崩溃后由内核释放锁。锁文件不含令牌，持久的刷新不可重试标记仍负责阻止崩溃后的重复交换。

## CodeStudio Next 接口

任务服务提供 `mcp/auth/discover`、`mcp/auth/login`、`mcp/auth/status`、`mcp/auth/logout`。参数为 `mcp_config`、`server`，login 额外接受 `login_timeout_ms`。这些方法使用现有异步操作协议，先返回操作 ID，再通过 `operation/finished` 返回结果。登录中发送 `operation/authorization`，包含操作 ID、`type: authorization`、授权 URL 和回调 URI；客户端可以展示链接或由用户操作打开浏览器。通知不包含 code、PKCE verifier 或令牌。

每个服务仍限一个活动操作。`operations/cancel` 可取消等待；断连后不自动登录或重放令牌交换。授权 URL 通知仅在当前连接发送，丢失通知时可查询操作，必要时明确取消后重新发起登录。详见[任务服务](task-service.md)和[机器契约](../protocol/task-service.schema.json)。

当前支持范围与官方规范核对记录见[协议依据](../research/mcp-extensions/oauth-protocol-notes.md)。这是明确范围的公开客户端实现，不表示所有 OAuth 扩展或任意身份服务均已适配；真实账号及 CodeStudio Next 浏览器交互需在对应环境验收。
