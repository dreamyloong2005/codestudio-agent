#pragma once
#include "codestudio/tools.hpp"

namespace codestudio {
// A user-granted, frozen build recipe. The model has no path, command or
// environment overrides. This adapter freezes a host-validated explicit recipe;
// generic isolated execution lives in WorkspaceExecution, not in this tool.
class WorkspaceBuild final : public ToolExtension {
public:
    WorkspaceBuild(const std::filesystem::path& manifest,
                   const std::filesystem::path& workspace,
                   const std::vector<std::filesystem::path>& protected_paths);
    ~WorkspaceBuild();
    Json binding() const;
    const std::filesystem::path& manifest_path() const;
    Json schemas() const override;
    void authorize(const ToolCall&) const override;
    ToolResult execute(const ToolCall&, const std::atomic_bool&) const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
