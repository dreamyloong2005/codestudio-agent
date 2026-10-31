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
have changed; `never` forbids general automatic request replay. It does not forbid
a new, explicit user action after inspecting durable state. JSON-RPC IDs only
correlate messages and are not idempotency keys. For `tasks/start` and
`tasks/resume`, additional `deduplication` metadata describes the narrower paired
`database_id` / `request_key` contract: an identical submission returns the
original receipt without executing again. Other operations do not gain this
behavior. `completion_result` exists for methods that return an operation
acceptance. `x-notifications` maps server notification names to parameter schemas.

Responses do not include their request's method. Validate the response envelope,
then use the correlated request's `x-methods[method].result` for its result.
For `operation/finished`, use the originating operation method's
`completion_result` when a result is present. Structured errors use `rpcError`.
`operations/get` supplies its originating method and the same completion shape.
The bounded in-memory history is scoped to `session_id`; a cache miss or changed
session never proves that a previous operation had no effects. Large cached
results explicitly carry `result: null`, `result_omitted`, `result_bytes` and
`result_refs`, while the original completion notification keeps its full result.

`initialize` remains read-only and reports `database_id: null` when state does
not exist. `state/initialize` explicitly creates an empty current-schema database
under runner admission locks and returns its identity; it never creates a task.
Save that identity and the request key before the first keyed submission. Keys
are client-generated nonsecret identifiers, never API keys or access tokens;
known environment secrets in a key are rejected before admission.
`requests/get` queries the durable receipt using both values. Receipts and keys
are retained for the lifetime of the database, with a 10,000-record limit that
rejects new keys rather than evicting old ones. The database schema is 8; older
initialized schemas are rejected without migration.

A keyed acceptance includes `durable_request` and `replayed`. Repeated keys
return the original operation/session identities, which may differ from the
current connection, and do not re-emit completion or text notifications. The
request is matched against its method, workspace and canonical explicit JSON
parameters: object key order is immaterial; array order, omitted fields and
explicit defaults remain significant. Configuration and credentials are not
reloaded for a duplicate. A durable `incomplete` receipt means only that its final
completion is absent; it proves neither current execution nor absence of effects.
Finished receipts use the same bounded completion shape as session snapshots.
Query the task and follow explicit recovery rules after interruption. Missing
receipts never authorize replay. Database identity catches replacement with a
different database, but cannot detect every rollback of the same database; do not
reuse old keys or automatically retry old requests after restoring a backup.

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
`validate_completion(method, completion)`, `validate_durable_request(receipt)` and
`validate_frame(frame, method=None)`.
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

## Sandbox resources

`sandboxes/list` is a read-only current-user resource query with optional `after`
and `limit`. `sandboxes/reclaim` is a foreground operation with an exact registered
`id` and optional boolean `dry_run`. Both use the host's private registry, rather
than service-workspace task state. They do not accept arbitrary cleanup paths,
adopt historical directories, signal processes, or change unknown task effects.
See [resource lifecycle](../docs/sandbox-resources.md) for states and limits.
