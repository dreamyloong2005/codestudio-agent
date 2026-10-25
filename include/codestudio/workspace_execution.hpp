#pragma once
#include "codestudio/types.hpp"
#include <memory>
#include <string_view>

namespace codestudio {
// Host-owned execution of a fixed, generic workspace recipe. Construction
// validates and copies every field; it neither runs code nor grants new access.
// Callers must obtain authorization and persist intent before execute().
// This is an internal C++ interface, not a third-party plugin ABI.
// The manifest and validated execution recipe use the same current format.
class WorkspaceExecution final {
public:
    WorkspaceExecution(const Json& recipe,
                       const std::filesystem::path& workspace,
                       const std::vector<std::filesystem::path>& protected_paths);
    ~WorkspaceExecution();
#ifdef _WIN32
    static constexpr std::string_view backend_id = "windows-lpac-build-v2";
#elif defined(__linux__)
    static constexpr std::string_view backend_id = "linux-namespace-build-v2";
#else
    static constexpr std::string_view backend_id = "unsupported";
#endif
    static void validate_available();
    // Imports declared inputs, supervises all descendants, and publishes a new
    // verified artifact bundle. No model, plugin, or tool dispatch is performed.
    ToolResult execute(const std::atomic_bool& cancelled) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
