#pragma once

#include "codestudio/types.hpp"
#include <memory>
#include <map>

namespace codestudio {
inline constexpr std::size_t extension_tool_limit = 128;
inline constexpr std::size_t extension_schema_byte_limit = 1024 * 1024;

struct ToolPolicy {
    std::filesystem::path workspace;
    bool allow_write = false;
    std::vector<std::filesystem::path> allowed_executables;
    std::vector<std::filesystem::path> protected_paths;
    std::size_t max_file_bytes = 1024 * 1024;
    std::chrono::milliseconds process_timeout{10000};
    std::size_t max_process_output_bytes = 65536;
    SandboxMode sandbox = SandboxMode::none;
};

// Extensions register frozen schemas; core policy and action receipts remain
// owned by the host. Loading an external process requires a separate grant.
class ToolExtension {
public:
    virtual ~ToolExtension() = default;
    virtual Json schemas() const = 0;
    virtual void authorize(const ToolCall&) const = 0;
    virtual ToolResult execute(const ToolCall&, const std::atomic_bool&) const = 0;
};

class ToolBroker {
public:
    explicit ToolBroker(ToolPolicy policy, std::vector<std::shared_ptr<ToolExtension>> extensions = {});
    Json schemas() const;
    // Throws on unknown tool, malformed arguments or denied capability. Must also
    // recheck inside execute immediately before any effect.
    void authorize(const ToolCall& call) const;
    ToolResult execute(const ToolCall& call, const std::atomic_bool& cancelled) const;
    const ToolPolicy& policy() const { return policy_; }
private:
    ToolPolicy policy_;
    std::vector<std::shared_ptr<ToolExtension>> extensions_;
    Json extension_schemas_ = Json::array();
    std::map<std::string,std::shared_ptr<ToolExtension>> extension_tools_;
};
}
