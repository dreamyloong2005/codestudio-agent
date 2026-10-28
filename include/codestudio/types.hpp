#pragma once

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace codestudio {
using Json = nlohmann::json;

struct ToolCall {
    std::string id;
    std::string name;
    Json arguments = Json::object();
};

struct ToolResult {
    bool ok = false;
    Json output = Json::object();
    std::string error;
    // none, applied, partial, unknown; an execution failure can still have effects.
    std::string effect = "none";
};

enum class SandboxMode { none, appcontainer, linux_namespace };

// Reject unsupported isolation before launching; never silently downgrade it.
void validate_sandbox_mode(SandboxMode mode);
std::string sandbox_mode_name(SandboxMode mode);

class ProcessPackage;
struct ProcessPackageLimits {
    unsigned processes = 1;
    std::uint64_t memory_bytes = 512ULL * 1024 * 1024;
    unsigned cpu_percent = 80;
};
struct ProcessRequest {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::chrono::milliseconds timeout{10000};
    std::size_t max_output_bytes = 65536;
    SandboxMode sandbox = SandboxMode::none;
    std::shared_ptr<const ProcessPackage> package;
    ProcessPackageLimits package_limits;
    // Host task correlation only; empty for pre-task discovery. Never exposed
    // in the sandbox environment or used as cleanup authorization.
    std::string task_id;
};

struct ProcessResult {
    int exit_code = -1;
    std::string output;
    bool timed_out = false;
    bool cancelled = false;
    bool output_truncated = false;
};

ProcessResult run_process(const ProcessRequest& request, const std::atomic_bool& cancelled);
}
