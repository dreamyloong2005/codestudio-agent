# Task service protocol 2

[task-service.schema.json](task-service.schema.json) is the machine-readable
Draft 2020-12 contract for the local JSON-RPC service. The human-readable
[service guide](../docs/task-service.md) defines lifecycle, authority and recovery
semantics. This directory contains no client UI or generated runtime dependency.

`x-methods` maps each method name to:

```json
{
  "params": {"$ref": "#/$defs/taskStartParams"},
  "result": {"$ref": "#/$defs/operationAccepted"},
  "execution": "operation",
  "retry": "never",
  "completion_result": {"$ref": "#/$defs/taskRunResult"}
}
```

`execution` is `query`, synchronous `control`, or asynchronous foreground
`operation`. `retry: query_only` permits another observation, whose result may
have changed; `never` forbids automatic request replay. It does not forbid a new,
explicit user action after inspecting durable state. JSON-RPC IDs only correlate
messages and are not idempotency keys. `completion_result` exists only for the
five methods that return an operation acceptance. `x-notifications` maps the
four server notification names to their parameter schemas.

Responses do not include their request's method. Validate the response envelope,
then use the correlated request's `x-methods[method].result` for its result.
For `operation/finished`, use the originating operation method's
`completion_result` when a result is present. Structured errors use `rpcError`.
`operations/get` supplies its originating method and the same completion shape.
The bounded in-memory history is scoped to `session_id`; a cache miss or changed
session never proves that a previous operation had no effects. Large cached
results explicitly carry `result: null`, `result_omitted`, `result_bytes` and
`result_refs`, while the original completion notification keeps its full result.

All `$ref` targets are local. Request fields and options are closed; result
records allow additive fields. Event payloads, tool receipts, provider bindings
and build recipes retain their own contracts and are intentionally open where
this transport schema cannot describe their entire domain. Schema validity does
not replace path, permission, checkpoint-binding, cron/timezone or cross-field
budget checks. A structurally valid request can still fail before or after an
effect; use structured error `outcome` and durable records when deciding recovery.

`maxLength` counts Unicode code points. `x-maxBytes` documents the separate UTF-8
byte limit enforced by the runtime. Standard JSON Schema validators ignore that
extension, so byte bounds need an additional check. The service also accepts only
integer JSON tokens for integer fields (`2.0` is rejected), although standard
JSON Schema regards mathematically integral numbers as integers. The test helper
applies both constraints without a custom schema language.

Validation uses the development-only dependency in
[tests/requirements.txt](../tests/requirements.txt):

```sh
python -m pip install -r tests/requirements.txt
python tests/contract_schema.py
python tests/client_contract_integration.py /absolute/path/to/codestudio-agent
```

`Contract` in [tests/contract_schema.py](../tests/contract_schema.py) exposes
`validate_request(frame)`, `validate_response(method, frame)`,
`validate_notification(frame, operation_method=None)`,
`validate_completion(method, completion)` and `validate_frame(frame, method=None)`.
Validation errors are standard `jsonschema.ValidationError` instances. The
static suite checks the Draft 2020-12 metaschema, resolves every reference, checks
runtime dispatch/notification coverage and exercises invalid wire boundaries.

## MCP OAuth and plugin operations

`mcp/auth/discover`, `mcp/auth/login`, `mcp/auth/status`, `mcp/auth/logout` and
`plugins/pack`, `plugins/install`, `plugins/lock`, `plugins/inspect` use the existing
foreground-operation acceptance, cancellation and completion contract. Filesystem
paths are relative to the service workspace. OAuth login emits
`operation/authorization` with a browser URL and loopback redirect URI; this is
a transient client notification, not a persisted task event. No token, code or
PKCE verifier appears on the wire. Clients handle the user interaction; the
headless runtime never opens a browser. Exact parameters are in the schema.

The new operations are explicit host requests, never model tools or scheduler
authorization. Installation does not execute package hooks or grant MCP access.
Cancellation does not undo a published package/lock or completed token exchange;
inspect status after an interrupted operation rather than replaying it blindly.
