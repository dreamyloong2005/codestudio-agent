# MCP OAuth implementation scope

Primary sources retrieved on 2026-10-25:

- [MCP 2025-11-25 Authorization](https://modelcontextprotocol.io/specification/2025-11-25/basic/authorization): protected-resource discovery, issuer selection, OAuth/OIDC discovery endpoint order, PKCE, resource indicators, access-token usage and preregistration.
- [RFC 9728](https://www.rfc-editor.org/rfc/rfc9728.html): OAuth protected resource metadata and the `resource_metadata` challenge parameter.
- [RFC 8414](https://www.rfc-editor.org/rfc/rfc8414.html): issuer validation and authorization server metadata well-known locations.
- [RFC 7636](https://www.rfc-editor.org/rfc/rfc7636.html): code verifier entropy, S256 challenge and authorization-code exchange binding.
- [RFC 8707](https://www.rfc-editor.org/rfc/rfc8707.html): `resource` in authorization and token requests.
- [RFC 8252](https://www.rfc-editor.org/rfc/rfc8252.html): public native client authorization and ephemeral loopback redirect ports.
- [RFC 9207](https://www.rfc-editor.org/rfc/rfc9207.html): authorization response issuer identification and exact `iss` verification when advertised.

The downloaded pages are retained beside this note as research evidence. They are data, not project instructions.

## Selected behavior

The MCP manifest supplies an exact resource endpoint, pinned issuer, preregistered public `client_id`, exact scopes and a current-user system credential name. No discovery result silently selects another issuer or expands scopes. The public client must be registered to accept `http://127.0.0.1:<ephemeral-port>/oauth/callback` redirects, as described by RFC8252. Dynamic registration, client metadata documents, confidential client authentication, device grants and scope escalation are not implemented in this milestone.

Discovery first issues a token-free GET to the resource. A 401 may advertise a quoted `resource_metadata` document; this implementation requires that document on the configured resource origin. Missing/404/405 metadata falls back to the resource-path well-known URI and then the origin-wide URI. Its exact `resource` must equal the configured endpoint and `authorization_servers` must contain the explicitly pinned issuer. Authorization server metadata uses RFC8414 and OIDC fallback paths in MCP order; the returned `issuer` must match exactly. PKCE S256 and response type `code` are required. Explicit metadata authentication methods must include `none`.

Login binds a temporary IPv4 loopback listener before reporting an authorization URL. The module never opens a browser. CodeStudio Next can receive the authorization notification and open that URL; CLI users can open it themselves. Each login has a fresh cryptographic state and 32-byte verifier, percent-encoded redirect and resource parameters, S256 challenge, strict callback Host/path/state checks and an issuer parameter check whenever advertised. Bad callbacks return a generic400 and cannot cause a token POST. The listener accepts at most16 requests, bounds callback headers to16KiB, and closes on completion/cancellation/timeout. The callback waiting budget is separately configurable; metadata and token HTTP use the configured request deadline.

Tokens live only in a dedicated OAuth namespace in Windows Credential Manager or Linux Secret Service, inaccessible through the ordinary `credential-*` interface and never used as a serialized Bearer value. A compact version2 record binds tokens to a hash of resource/issuer/client/credential/scopes/HTTP exception, token endpoint, expiration and refresh state. The combined record is limited to2048 bytes by the current secure store contract; oversized authorization results fail instead of falling back to plaintext. No OAuth SQLite table, checkpoint token, environment fallback, login UI or automatic store unlock is added.

Refresh happens once before a new explicitly granted MCP operation. It uses the pinned token endpoint and sends resource/client/scope binding, serializes against login/logout/other refreshes, and persists `refresh_blocked` before dispatch. Any crash, network ambiguity, malformed response, token rejection or failed secure-store update after that fence requires explicit login rather than replaying a possibly rotated refresh token. Successful refresh atomically replaces the secure-store item and preserves a prior refresh token when the response omits a replacement. An access token expiring in30 seconds or less needs a usable refresh token; otherwise explicit login is required. Tokens require explicit bounded positive `expires_in`, Bearer type and any returned scope must equal the requested scope set.

The Windows lock is a named kernel mutex bound to current-user identity and credential name. Linux uses a UID/name-bound abstract Unix socket in the current network namespace, providing cross-process lifetime and no stale lock files; the lock is coordination, not an access-control barrier. Processes deliberately placed in different Linux network namespaces do not share this mutex and must not rotate the same credential concurrently. Native secure-store ownership remains authoritative.

Runtime does not react to a tool401 by logging in or refreshing/replaying `tools/call`. Existing unknown-effect handling remains authoritative. Token values known during login/refresh are supplied to the caller's redactor before validation failures; both access and refresh tokens are available for normal run and saved-approval redaction even after expiry or an uncertain refresh. Status returns only metadata/state. Logout removes the local OAuth record only; it does not claim authorization-server token revocation.

HTTP is bounded to1MiB response bodies/64KiB headers, verifies HTTPS certificates, does not follow redirects, and disables default credentials, cookies and proxies. Explicit literal-loopback HTTP is accepted only for owned test/development endpoints. Authorization/token/metadata endpoints currently use the existing query-free HTTP endpoint grammar; this implementation does not claim compatibility with servers requiring query-bearing endpoints or cross-origin protected-resource metadata documents. It is a constrained preregistered-public-client MCP OAuth implementation, not a claim of every optional OAuth mechanism.

## Verification expectations

Use synthetic owned loopback resource/authorization servers and temporary dedicated OAuth credentials, with private Secret Service on Linux. Tests must independently check PKCE, state/issuer rejection, exact resource/scope, no token discovery/header leak, metadata refusal before token POST, secret redaction, refresh rotation and concurrency, refresh ambiguity no-replay, namespace separation, local logout/status, cancellation and callback cleanup. No live third-party login is required or implied by these synthetic tests. Root runs the native Windows/Linux builds and runtime tests and records actual results separately.
