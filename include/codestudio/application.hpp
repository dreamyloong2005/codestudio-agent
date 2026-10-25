#pragma once

#include "codestudio/types.hpp"
#include <functional>
#include <string_view>

namespace codestudio {
struct CommandResult {
    // help/version return display text; all other commands return JSON data.
    Json value;
    int exit_code = 0;
};

struct CommandCallbacks {
    // Provisional UTF-8 text, with operation credentials redacted even when
    // split across provider deltas. Synchronous; the view lasts only this call.
    // Terminal escaping and wire framing belong to the caller.
    std::function<void(std::string_view)> on_text;
    // Login URL notification only; never tokens/code/verifier. Caller chooses
    // how to present it. The runtime never opens a browser automatically.
    std::function<void(const Json&)> on_authorization;
};

// argv includes the program name. The caller owns cancellation and callbacks.
// Does not print. Results and thrown command error messages are redacted within
// the operation's credential scope. Permissions, locks and resume checks are
// shared by the CLI and local service through this execution path.
CommandResult execute_command(const std::vector<std::string>& argv,
                              const std::atomic_bool& cancelled,
                              const CommandCallbacks& callbacks = {});

// Scheduler dispatch takes persistent authorization only from the selected job.
// Claims run inside the same workspace/database locks as normal continuation.
CommandResult execute_scheduled_job(const std::filesystem::path& workspace,
                                   const std::filesystem::path& database,
                                   const std::string& job_id, const std::string& owner,
                                   std::int64_t generation, const std::atomic_bool& cancelled);
// Recovers owned jobs and atomically materializes due cron occurrences under
// the ordinary execution locks, then returns the current queue candidates.
Json prepare_scheduler_jobs(const std::filesystem::path& workspace,
                            const std::filesystem::path& database,
                            const std::string& owner,std::int64_t generation);

// Apply operation-scoped credential redaction; outside an operation this only
// knows CODESTUDIO_API_KEY. Resolved native/custom keys never enter a global cache.
Json application_redact(Json value);
}
