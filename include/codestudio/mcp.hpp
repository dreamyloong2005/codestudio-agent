#pragma once
#include "codestudio/tools.hpp"
#include "codestudio/mcp_oauth.hpp"
#include <functional>

namespace codestudio {
// A frozen, fully validated manifest. Endpoint selection, credential references
// and the saved authorization binding all come from these same bytes. Creating
// a plan performs no process launch, credential lookup or network operation.
class McpPlan {
public:
    McpPlan(const std::filesystem::path& manifest, const std::filesystem::path& workspace,
            const std::vector<std::string>& granted_servers, SandboxMode sandbox = SandboxMode::none);
    const std::string& snapshot() const;
    std::vector<std::string> credential_references() const;
    std::vector<McpOAuthConfig> oauth_configurations() const;
    Json plugin_bindings() const;
    std::vector<std::filesystem::path> protected_paths() const;
    std::vector<std::shared_ptr<ToolExtension>> connect(
        const std::function<std::string(const std::string&)>& resolver,
        const std::atomic_bool& cancelled, std::size_t reserved_tools = 0,
        std::size_t reserved_schema_bytes = 0) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

// Reads one explicitly selected OAuth configuration without launching a server
// or reading credentials. Used by both CLI and task-service connection commands.
McpOAuthConfig mcp_oauth_configuration(const std::filesystem::path&, const std::string& server);
// Parse the OAuth section of an already frozen server; no I/O.
McpOAuthConfig mcp_oauth_configuration(const Json& server);

// Version 2 manifest only. All servers are disabled until their id is explicitly
// granted by the user. No workspace auto-discovery or implicit package runner.
// {"version":2,"servers":[{"id":"example","command":"absolute executable",
//   "args":[],"tools":["echo"],"timeout_ms":10000}]}
// tools is a required exact allowlist, never inferred from server annotations.
// Sibling extensions reserve their count and serialized schema-array bytes.
// Both reservation arguments are zero when no sibling extension is present.
std::vector<std::shared_ptr<ToolExtension>> load_mcp_extensions(
    const std::filesystem::path& manifest, const std::filesystem::path& workspace,
    const std::vector<std::string>& granted_servers, const std::atomic_bool& cancelled,
    SandboxMode sandbox = SandboxMode::none, std::size_t reserved_tools = 0,
    std::size_t reserved_schema_bytes = 0);
}
