# CodeStudio-Agent

CodeStudio-Agent is a headless agent runtime and harness. It connects model reasoning to explicitly authorized tools while retaining execution and recovery records.

## Language

**Plugin package**: A distributable, identified release of an MCP process server and its declared payload. Installing one does not grant permission to run it.

**Plugin lock**: The exact selection of one plugin release and its content identity for a configured MCP server. A version label alone is not a lock.

**MCP grant**: The user's explicit permission to connect a named server and expose its configured tool allowlist for an operation.

**OAuth connection**: A user's authorization of a client to access one MCP resource through a specific authorization server and scope set. It is separate from an MCP grant.

**Authorization binding**: The frozen configuration, permissions and budgets under which a task may execute or resume.

**Unknown effect**: A dispatched operation whose result cannot be established. It does not authorize a retry.
