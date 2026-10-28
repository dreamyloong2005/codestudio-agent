# CodeStudio Agent

CodeStudio Agent is a headless agent runtime and harness. It connects model reasoning to explicitly authorized tools while retaining execution and recovery records.

## Language

**Plugin package**: A distributable, identified release of an MCP process server and its declared payload. Installing one does not grant permission to run it.

**Plugin lock**: The exact selection of one plugin release and its content identity for a configured MCP server. A version label alone is not a lock.

**MCP grant**: The user's explicit permission to connect a named server and expose its configured tool allowlist for an operation.

**OAuth connection**: A user's authorization of a client to access one MCP resource through a specific authorization server and scope set. It is separate from an MCP grant.

**Authorization binding**: The frozen configuration, permissions and budgets under which a task may execute or resume.

**Unknown effect**: A dispatched operation whose result cannot be established. It does not authorize a retry.

## Platform status

macOS stage 1 targets native Apple Silicon: LLVM 22.1.8 with a matching timezone-enabled libc++, XMake, ordinary process execution, CLI/stdio service, model/MCP transports, and explicit recovery. Build the runtime with `scripts/build-macos-libcxx.py`, then use `scripts/verify-macos.sh` with `CODESTUDIO_LLVM_ROOT` and `CODESTUDIO_LIBCXX_ROOT`. The deployment target is macOS 15.0; native Debug/Release validation passed on macOS 26.5.2; older systems were not runtime-tested. See [macOS](docs/macos.md) and [validation](docs/validation.md) for the actual evidence.

macOS does not yet provide native OAuth/Keychain storage, detached workers, supervision, LaunchAgent, OS isolation, isolated workspace builds, resource reclamation, or signed/notarized distribution. Capability declarations and rejected requests retain those boundaries. Schema 7 and protocol/checkpoint/config version 2 remain unchanged.
