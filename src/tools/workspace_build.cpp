#include "codestudio/workspace_build.hpp"
#include "codestudio/build_manifest.hpp"
#include "codestudio/build_workspace.hpp"
#include "codestudio/workspace_execution.hpp"
#include "codestudio/store.hpp"
#include "../provider/protocol.hpp"
#include <stdexcept>

namespace codestudio {
namespace fs = std::filesystem;

struct WorkspaceBuild::Impl {
    fs::path manifest;
    std::string snapshot;
    Json binding;
    std::unique_ptr<WorkspaceExecution> execution;
};

WorkspaceBuild::WorkspaceBuild(const fs::path& manifest, const fs::path& workspace,
    const std::vector<fs::path>& protected_paths) : impl_(std::make_unique<Impl>()) {
    auto& plan = *impl_;
    plan.manifest = fs::absolute(manifest).lexically_normal();
    plan.snapshot = read_build_manifest_snapshot(plan.manifest);
    const auto data = protocol::strict_json(plan.snapshot);
    auto recipe = normalize_build_manifest(data);
    auto protected_inputs = protected_paths;
    protected_inputs.push_back(plan.manifest);
    plan.execution = std::make_unique<WorkspaceExecution>(recipe, workspace, protected_inputs);
    plan.binding = {{"version",2},{"path",path_utf8(plan.manifest)},{"manifest",plan.snapshot},
        {"backend",WorkspaceExecution::backend_id},{"network","none"},{"export","new-atomic-bundle"},
        {"recipe",std::move(recipe)}};
}
WorkspaceBuild::~WorkspaceBuild() = default;
const fs::path& WorkspaceBuild::manifest_path() const { return impl_->manifest; }
Json WorkspaceBuild::binding() const { return impl_->binding; }
Json WorkspaceBuild::schemas() const {
    return Json::array({{{"type","function"},{"function",{{"name","build_workspace"},
        {"description","Build a private workspace copy using the granted fixed recipe; export declared artifacts as a new bundle. No command/path overrides."},
        {"parameters",{{"type","object"},{"properties",Json::object()},{"required",Json::array()},{"additionalProperties",false}}}}}}});
}
void WorkspaceBuild::authorize(const ToolCall& call) const {
    if (call.name != "build_workspace" || !call.arguments.is_object() || !call.arguments.empty())
        throw std::invalid_argument("build_workspace takes an empty object and no overrides");
    if (read_build_manifest_snapshot(impl_->manifest) != impl_->snapshot)
        throw std::runtime_error("Build manifest changed since authorization");
    WorkspaceExecution::validate_available();
}
ToolResult WorkspaceBuild::execute(const ToolCall& call, const std::atomic_bool& cancelled) const {
    authorize(call);
    return impl_->execution->execute(cancelled);
}
}
