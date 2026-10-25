#pragma once

#include "codestudio/provider.hpp"
#include "codestudio/store.hpp"
#include "codestudio/tools.hpp"

namespace codestudio {
struct RunOptions {
    std::string prompt;
    int max_turns = 8;
    std::size_t max_context_bytes = 4 * 1024 * 1024;
    std::vector<std::string> secrets;
    Json binding = Json::object(); // Nonsecret provider/plugin configuration.
    std::string resume_task_id;
    std::string scheduler_job_id;
    std::string scheduler_claim_token;
};

Json run_agent(Store& store, ModelProvider& provider, const ToolBroker& tools,
               const RunOptions& options, const std::atomic_bool& cancelled);
// Pure initial state/configuration snapshot for explicitly recurring tasks.
// Requires discovered tool schemas; never calls a model or executes a tool.
Json prepare_initial_checkpoint(ModelProvider& provider, const ToolBroker& tools,
                                const RunOptions& options);
Json tool_result_json(const ToolResult& result);
Json redact(Json value, const std::vector<std::string>& secrets);
Json policy_binding(const ToolPolicy& policy);
void validate_resume_state(const Json& state, const Json& task, bool allow_waiting_proposal = false);
}
