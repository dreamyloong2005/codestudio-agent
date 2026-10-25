#include "codestudio/build_proposal.hpp"
#include "codestudio/build_manifest.hpp"
#include "codestudio/digest.hpp"
#include "codestudio/store.hpp"
#include "codestudio/workspace_execution.hpp"
#include <set>
#include <stdexcept>
#include <utility>

namespace codestudio {
namespace {
bool contains_secret(const Json& value, const std::vector<std::string>& secrets) {
    const auto sensitive = [&](const std::string& text) {
        for (const auto& secret : secrets)
            if (!secret.empty() && text.find(secret) != std::string::npos) return true;
        return false;
    };
    if (value.is_string()) return sensitive(value.get_ref<const std::string&>());
    if (value.is_array()) {
        for (const auto& item : value) if (contains_secret(item, secrets)) return true;
    } else if (value.is_object()) {
        for (const auto& [name, item] : value.items())
            if (sensitive(name) || contains_secret(item, secrets)) return true;
    }
    return false;
}
}

Json inspect_build_proposal(const Json& arguments, const std::filesystem::path& workspace,
                           const std::vector<std::filesystem::path>& protected_paths) {
    if (!arguments.is_object() || arguments.size() != 2 ||
        !arguments.contains("manifest") || !arguments["manifest"].is_object() ||
        !arguments.contains("reason") || !arguments["reason"].is_string())
        throw std::invalid_argument("propose_build requires exactly manifest and reason");
    if (arguments.dump().size() > build_proposal_argument_limit)
        throw std::invalid_argument("Build proposal arguments exceed 64 KiB");
    const auto& reason = arguments["reason"].get_ref<const std::string&>();
    if (reason.empty() || reason.size() > 4096 || reason.find('\0') != std::string::npos)
        throw std::invalid_argument("Build proposal reason must contain 1..4096 bytes without NUL");
    const auto& manifest = arguments["manifest"];
    if (!manifest.contains("version") || !manifest["version"].is_number_integer() || manifest["version"] != 2)
        throw std::invalid_argument("Build proposals require a version 2 manifest");
    WorkspaceExecution::validate_available();
    const auto canonical = std::filesystem::canonical(workspace);
    auto recipe = normalize_build_manifest(manifest);
    if (protected_paths.size() > build_proposal_protected_path_limit)
        throw std::invalid_argument("Build proposal has too many host-protected paths");
    std::set<std::string> protected_names;
    for (const auto& path : protected_paths) {
        const auto value = path_utf8(std::filesystem::weakly_canonical(std::filesystem::absolute(path)));
        if (value.empty() || value.size() > 32768)
            throw std::invalid_argument("Build proposal protected path exceeds size limit");
        protected_names.insert(value);
    }
    // Construction validates the proposed plan, but deliberately never opens
    // a mount, copies a source file or runs an executable.
    const WorkspaceExecution checked(recipe, canonical, protected_paths);
    Json proposal{{"version",2},{"status","pending_approval"},{"workspace",path_utf8(canonical)},
        {"reason",reason},{"manifest",manifest},
        {"recipe",std::move(recipe)},{"protected_paths",protected_names},{"backend",WorkspaceExecution::backend_id},
        {"network","none"},{"export","new-atomic-bundle"}};
    // nlohmann::json uses ordered object keys; this is the exact UTF-8 encoding
    // reviewed and compared by this version of the proposal protocol.
    const auto bytes = proposal.dump();
    proposal["digest"] = sha256_bytes(bytes);
    if (proposal.dump().size() > build_proposal_output_limit)
        throw std::invalid_argument("Expanded build proposal exceeds 128 KiB");
    return proposal;
}

Json validate_build_proposal_receipt(const Json& arguments, const Json& output,
                                    const std::filesystem::path& workspace,
                                    const std::vector<std::filesystem::path>& protected_paths) {
    if (!output.is_object() || output.dump().size() > build_proposal_output_limit)
        throw std::invalid_argument("Invalid build proposal receipt");
    auto current = inspect_build_proposal(arguments, workspace, protected_paths);
    if (current != output)
        throw std::runtime_error("Build proposal content or normalized plan has changed; a new proposal is required");
    return current;
}

BuildProposal::BuildProposal(const std::filesystem::path& workspace,
                             const std::vector<std::filesystem::path>& protected_paths,
                             std::vector<std::string> secrets)
    : workspace_(std::filesystem::canonical(workspace)), protected_paths_(protected_paths),
      secrets_(std::move(secrets)) {}

Json BuildProposal::schemas() const {
    return Json::array({{{"type","function"},{"function",{{"name","propose_build"},
        {"description","Propose a version-2 isolated build and wait for explicit host approval or rejection. The tool result arrives after the decision with the execution receipt, if approved. A further attempt requires a new proposal. Source and compiler bytes are not snapshotted until approval."},
        {"parameters",{{"type","object"},{"properties",{
            {"manifest",{{"type","object"},{"description","Version 2 build manifest: inputs and outputs are relative path arrays; mounts declare name, absolute source and relative inputs; declare explicit steps (executable, arguments, optional cwd); artifact_directory and optional limits/environment define the requested publication and resource policy. Use declared ${workspace}, ${home}, ${temp}, or ${mount_name} roots."},{"additionalProperties",true}}},
            {"reason",{{"type","string"},{"minLength",1},{"maxLength",4096}}}}},
            {"required",Json::array({"manifest","reason"})},{"additionalProperties",false}}}}}}});
}

Json BuildProposal::inspect(const ToolCall& call) const {
    if (call.name != "propose_build") throw std::invalid_argument("Unknown build proposal tool");
    if (contains_secret(call.arguments, secrets_))
        throw std::invalid_argument("Build proposal contains a configured credential and cannot be persisted");
    auto proposal = inspect_build_proposal(call.arguments, workspace_, protected_paths_);
    if (contains_secret(proposal, secrets_))
        throw std::invalid_argument("Build proposal would require credential redaction and cannot be approved");
    return proposal;
}

void BuildProposal::authorize(const ToolCall& call) const { (void)inspect(call); }

ToolResult BuildProposal::execute(const ToolCall& call, const std::atomic_bool& cancelled) const {
    if (cancelled.load()) return {false, {{"cancelled",true}}, "Build proposal cancelled", "none"};
    try {
        auto proposal = inspect(call);
        if (cancelled.load()) return {false, {{"cancelled",true}}, "Build proposal cancelled", "none"};
        return {true, std::move(proposal), {}, "none"};
    } catch (const std::exception& error) {
        return {false, Json::object(), error.what(), "none"};
    }
}
}
