#include "codestudio/build_approval.hpp"
#include "codestudio/build_proposal.hpp"
#include "codestudio/workspace_execution.hpp"
#include <stdexcept>

namespace codestudio {
namespace {
std::vector<std::filesystem::path> protection(const Json& output) {
    const auto& paths = output.at("protected_paths");
    if (!paths.is_array() || paths.size() > build_proposal_protected_path_limit)
        throw std::runtime_error("Invalid proposal protection policy");
    std::vector<std::filesystem::path> result;
    for (const auto& value : paths) {
        if (!value.is_string() || value.get_ref<const std::string&>().size() > 32768)
            throw std::runtime_error("Invalid proposal protection path");
        auto path = utf8_path(value.get<std::string>());
        if (!path.is_absolute()) throw std::runtime_error("Proposal protection paths must be absolute");
        result.push_back(std::move(path));
    }
    return result;
}
}

Json review_build_proposal(const Json& record, const std::filesystem::path& workspace) {
    if (record.at("workspace") != path_utf8(workspace))
        throw std::runtime_error("Build proposal belongs to another workspace");
    if (application_redact(record) != record)
        throw std::runtime_error("Build proposal requires credential redaction and cannot be reused");
    const auto& output = record.at("result").at("output");
    const auto proposal = validate_build_proposal_receipt(record.at("arguments"), output,
                                                        workspace, protection(output));
    const auto& decision = record.at("decision");
    if (!decision.is_null() && decision.at("digest") != proposal.at("digest"))
        throw std::runtime_error("Stored build decision differs from the proposal");
    return {{"task_id",record.at("task_id")},{"action_id",record.at("action_id")},
            {"proposal",proposal},{"decision",decision}};
}

CommandResult decide_build_proposal(Store& store, const std::filesystem::path& workspace,
    const std::filesystem::path& database, const std::string& task, const std::string& action,
    const std::string& digest, bool approve, const std::atomic_bool& cancelled) {
    const auto reviewed = review_build_proposal(store.build_proposal(task, action), workspace);
    if (reviewed.at("proposal").at("digest") != digest)
        throw std::runtime_error("Approval digest does not match the reviewed build proposal");
    if (cancelled.load()) throw std::runtime_error("Build decision cancelled before authorization");

    // Retain the source task's protected manifests as well as this executor's
    // current state paths. A proposal never removes host protection policy.
    auto protected_paths = protection(reviewed.at("proposal"));
    protected_paths.push_back(workspace / ".codestudio");
    protected_paths.push_back(database);
    for (const auto* suffix : {"-wal","-shm","-journal",".run.lock",".scheduler.run.lock",".supervisor.run.lock"})
        protected_paths.push_back(utf8_path(path_utf8(database) + suffix));
    WorkspaceExecution execution(reviewed.at("proposal").at("recipe"), workspace, protected_paths);
    if (approve) WorkspaceExecution::validate_available();
    // The unique claim, task, prepared action and events commit together before
    // dispatch. Any older decision can only be queried, never dispatched again.
    const auto claimed = store.decide_build_proposal(task, action, digest, approve, path_utf8(workspace));
    const auto& decision = claimed.at("decision");
    if (approve && claimed.at("decision_created").get<bool>()) {
        const auto execution_task = decision.at("execution_task_id").get<std::string>();
        const auto execution_action = decision.at("execution_action_id").get<std::string>();
        store.dispatch_action(execution_action);
        ToolResult receipt;
        try { receipt = execution.execute(cancelled); }
        catch (const std::exception& error) { receipt = {false,Json::object(),error.what(),"unknown"}; }
        auto body = application_redact({{"ok",receipt.ok},{"output",receipt.output},
                                       {"error",receipt.error},{"effect",receipt.effect}});
        receipt.output = std::move(body.at("output"));
        receipt.error = body.at("error").get<std::string>();
        store.finish_action(execution_action, receipt);
        const bool uncertain = receipt.effect == "unknown" || receipt.effect == "partial";
        const auto status = uncertain ? "needs_attention" : receipt.ok ? "completed" :
                            cancelled.load() ? "cancelled" : "failed";
        store.finish_task(execution_task, status, receipt.ok ? "Approved isolated build completed" :
                          uncertain ? "Build outcome requires inspection; approval will not replay" :
                                      "Approved build did not succeed; create a new proposal to try again");
    }
    auto result = Json{{"proposal",review_build_proposal(store.build_proposal(task,action),workspace)},
                       {"execution",nullptr},{"state_database",path_utf8(database)}};
    int exit_code = 0;
    if (approve) {
        result["execution"] = store.task(decision.at("execution_task_id").get<std::string>());
        const auto& executed = result.at("execution");
        if (executed.at("workspace") != path_utf8(workspace) || executed.at("provider") != "approved-build")
            throw std::runtime_error("Approved build execution identity is inconsistent");
        const auto status = executed.at("status").get<std::string>();
        exit_code = status == "completed" ? 0 : status == "cancelled" ? 130 : 1;
    }
    return {application_redact(std::move(result)),exit_code};
}
}
