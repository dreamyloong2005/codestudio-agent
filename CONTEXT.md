# CodeStudio Agent

CodeStudio Agent is a headless agent runtime and harness. It connects model reasoning to explicitly authorized tools while retaining execution and recovery records.

## Language

**Plugin package**: A distributable, identified release of an MCP process server and its declared payload. Installing one does not grant permission to run it.

**Plugin lock**: The exact selection of one plugin release and its content identity for a configured MCP server. A version label alone is not a lock.

**MCP grant**: The user's explicit permission to connect a named server and expose its configured tool allowlist for an operation.

**OAuth connection**: A user's authorization of a client to access one MCP resource through a specific authorization server and scope set. It is separate from an MCP grant.

**Authorization binding**: The frozen configuration, permissions and budgets under which a task may execute or resume.

**Unknown effect**: A dispatched operation whose result cannot be established. It does not authorize a retry.

## Current follow-up plan

The [macOS follow-up plan](docs/macos-follow-up.md) tracks source synchronization, schema-8 native Debug/Release verification, package/install acceptance, fixed-runtime real-login acceptance and an actual ARM64 CI run. Execution evidence and per-item status are recorded in [the macOS follow-up receipts](research/macos-durable-requests/README.md). The plan and tooling do not themselves establish test, real-login or CI success, or authorize formal publication. Preserve existing login fixtures and historical evidence. UI remains in CodeStudio Next, local-model development is paused, and native isolation and formal distribution remain deferred.

## Platform status

macOS targets native Apple Silicon: LLVM 22.1.8 with a matching timezone-enabled libc++, XMake, ordinary process execution, CLI/stdio service, model/MCP transports, and explicit recovery. Build the runtime with `scripts/build-macos-libcxx.py`, then use `scripts/verify-macos.sh` with `CODESTUDIO_LLVM_ROOT` and `CODESTUDIO_LIBCXX_ROOT`. The deployment target is macOS 15.0; native Debug/Release validation passed on macOS 26.5.2; older systems were not runtime-tested. Keychain/OAuth storage, detached workers, bounded supervision and LaunchAgent registration are implemented. Relocatable packages and user-level installation have local delivery evidence; see [macOS](docs/macos.md) and [validation](docs/validation.md) for the actual scope.

macOS real-login acceptance remains pending. Native OS isolation, isolated workspace execution and resource reclamation remain disabled; safe workspace preparation alone does not establish isolation. Developer ID/notarization tooling is implemented, but formal signing, notarization and independent clean-Mac acceptance are deferred. Hosted CI results are separate from local verification. Windows/WSL regression of the shared changes is recorded in [the current evidence directory](research/cross-platform-regression/README.md). Capability declarations and rejected requests retain these boundaries. State now uses schema 8 for durable request identities and receipts; prior databases are rejected without migration. Protocol/checkpoint/config version 2 remains current.
