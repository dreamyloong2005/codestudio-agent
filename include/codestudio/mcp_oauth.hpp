#pragma once
#include "codestudio/types.hpp"
#include <functional>

namespace codestudio {
// Explicit public client registration; scopes never expand from discovery data.
// resource is the frozen MCP endpoint, issuer is an explicitly trusted AS.
struct McpOAuthConfig {
    std::string resource;
    std::string issuer;
    std::string client_id;
    std::string credential;
    std::vector<std::string> scopes;
    bool allow_http_loopback = false;
    std::chrono::milliseconds timeout{10000};
};
struct McpOAuthToken {
    std::string access_token;
    // Include both access and refresh tokens in the operation's redactor.
    std::vector<std::string> secrets;
};
using McpOAuthNotice = std::function<void(const Json&)>;
using McpOAuthRemember = std::function<void(const std::string&)>;

// Static validation, no credential or network I/O. Errors never echo input.
void validate_mcp_oauth(const McpOAuthConfig&);
Json discover_mcp_oauth(const McpOAuthConfig&, const std::atomic_bool& cancelled);
// Starts a temporary IPv4 loopback callback listener, reports the authorization
// URL through notice, then waits within config.timeout. Never opens a browser.
Json login_mcp_oauth(const McpOAuthConfig&, const McpOAuthNotice& notice,
                    const std::atomic_bool& cancelled, const McpOAuthRemember& remember = {},
                    std::chrono::milliseconds login_timeout = std::chrono::milliseconds(300000));
// Local secure-store inspection/removal, no network or token output.
Json status_mcp_oauth(const McpOAuthConfig&, const std::atomic_bool& cancelled);
Json logout_mcp_oauth(const McpOAuthConfig&, const std::atomic_bool& cancelled);
// Called once before an MCP operation; never refresh in response to tools/call
// errors. A refresh with uncertain outcome requires an explicit new login.
McpOAuthToken resolve_mcp_oauth(const McpOAuthConfig&, const std::atomic_bool& cancelled,
                              const McpOAuthRemember& remember = {});
// Stable nonsecret full-configuration key for an operation's pinned token map.
std::string mcp_oauth_token_key(const McpOAuthConfig&);
// Reads redaction material without network/refresh, even after expiry or an
// uncertain refresh. Missing/locked storage never supplies a token implicitly.
std::vector<std::string> mcp_oauth_secrets(const McpOAuthConfig&);
}
