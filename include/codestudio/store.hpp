#pragma once

#include "codestudio/types.hpp"
#include <memory>
#include <optional>
#include <stdexcept>

struct sqlite3;
namespace codestudio {
struct SchedulerStartFence {
    std::int64_t stop_epoch;
    std::int64_t generation;
};
struct SupervisorRunOptions {
    std::string id;
    std::string owner;
    std::string workspace;
    std::string process_identity;
    std::int64_t pid;
    int poll_ms;
    int max_restarts;
    int restart_delay_ms;
};
struct SupervisorRunState {
    std::string phase;
    int restarts = 0;
    std::int64_t worker_pid = 0;
    std::int64_t generation = 0;
    std::int64_t next_restart_at_ms = 0;
    std::optional<std::int64_t> last_exit_code;
    std::string exit_reason;
};
std::string new_id();
std::string path_utf8(const std::filesystem::path& path);
std::filesystem::path utf8_path(const std::string& text);

// One active runner per state database. OS ownership is released on process exit.
class RunBusy : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RunLock {
public:
    explicit RunLock(const std::filesystem::path& database);
    ~RunLock();
    RunLock(const RunLock&) = delete;
    RunLock& operator=(const RunLock&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Store {
public:
    explicit Store(const std::filesystem::path& database);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    std::string create_task(const std::string& prompt, const std::string& workspace,
                            const std::string& provider);
    // waiting_approval pauses on an unconsumed proposal and requires its exact
    // resumable checkpoint; it emits task.waiting_approval, not task.finished.
    void finish_task(const std::string& id, const std::string& status, const std::string& summary,
                     const Json* checkpoint_state = nullptr);
    void event(const std::string& task, const std::string& kind, const Json& payload);
    std::string prepare_action(const std::string& task, const ToolCall& call);
    void dispatch_action(const std::string& action);
    void finish_action(const std::string& action, const ToolResult& result,
                       const Json* checkpoint_state = nullptr);
    void deny_action(const std::string& task, const ToolCall& call, const std::string& reason,
                     const Json* checkpoint_state = nullptr);
    // Checkpoint state is already redacted by the kernel. Binding is immutable.
    void save_checkpoint(const std::string& task, const Json& state, const Json& binding);
    Json checkpoint(const std::string& task) const;
    // Caller holds workspace/database locks. Requires recover before resuming a
    // crashed running task; rejects uncertain actions and mismatched bindings.
    void resume_task(const std::string& task, const Json& expected_binding,
                     const std::string& scheduler_job = {}, const std::string& claim_token = {});
    std::int64_t checkpoint_revision(const std::string& task) const;
    Json schedule_resume(const std::string& task, const Json& resume_argv, const Json& expected_binding,
                         std::int64_t expected_revision, std::int64_t due_ms, std::int64_t deadline_ms,
                         int max_attempts = 8, std::int64_t retry_delay_ms = 1000);
    Json schedules(std::int64_t before = 0, std::size_t limit = 100) const;
    Json pending_schedules() const;
    Json schedule(const std::string& id) const;
    // Requires scheduler ownership and workspace/database runner locks. Returns
    // null when not runnable; a returned job includes its unique claim_token.
    Json claim_schedule(const std::string& id, const std::string& owner,
                        std::int64_t generation, std::int64_t now_ms);
    // Only releases a claimed job that has not entered the task runner.
    void finish_scheduler_claim(const std::string& id, const std::string& claim_token,
                                const std::string& reason, std::int64_t now_ms);
    Json cancel_schedule(const std::string& id, std::int64_t now_ms, bool active_only = false);
    // Never recovers unrelated foreground tasks. Caller owns all runner locks.
    Json recover_schedules(const std::string& owner, std::int64_t generation, std::int64_t now_ms);
    // Compare both fences in the registration transaction; a stopped or replaced
    // worker must never be resurrected by an already dispatched restart.
    std::int64_t start_scheduler(const std::string& owner, std::int64_t pid, std::int64_t now_ms,
                                std::optional<SchedulerStartFence> expected = std::nullopt);
    bool heartbeat_scheduler(const std::string& owner, std::int64_t now_ms);
    Json stop_scheduler(std::optional<SchedulerStartFence> expected = std::nullopt);
    void finish_scheduler(const std::string& owner);
    Json scheduler() const;
    // Caller holds the supervisor admission lock. Registration and replacement
    // of any abandoned lifecycle are fenced against the same scheduler snapshot.
    void start_supervisor(const SupervisorRunOptions& options, SchedulerStartFence expected,
                          std::int64_t now_ms);
    // Only the latest active lifecycle and its owner may update. Every state
    // transition and its immutable event commit together; heartbeats emit none.
    bool update_supervisor(const std::string& id, const std::string& owner,
                           const SupervisorRunState& state, std::int64_t now_ms,
                           const std::string& event_kind);
    bool heartbeat_supervisor(const std::string& id, const std::string& owner, std::int64_t now_ms);
    Json supervisor() const;
    // A recurring authorization freezes one fresh ready checkpoint. Each due
    // occurrence atomically creates its task, checkpoint and scheduled job.
    Json create_cron(const std::string& name, const std::string& expression,
                     const std::string& timezone, const std::string& prompt,
                     const std::string& provider, const Json& resume_argv,
                     const Json& initial_checkpoint, std::int64_t deadline_ms,
                     int max_attempts, std::int64_t retry_delay_ms,
                     std::int64_t max_runs, std::int64_t now_ms);
    Json crons(std::int64_t before = 0, std::size_t limit = 100) const;
    Json cron(const std::string& id) const;
    Json control_cron(const std::string& id, const std::string& action, std::int64_t now_ms);
    // Caller owns the workspace and database runner locks and current daemon
    // generation. Missed slots coalesce, active occurrences never overlap.
    Json materialize_crons(const std::string& workspace, const std::string& owner,
                           std::int64_t generation, std::int64_t now_ms);
    bool has_running_tasks() const;
    Json list_tasks() const;
    Json task(const std::string& id) const;
    Json events(const std::string& id) const;
    // Exact, bounded read of a successful, side-effect-free propose_build receipt.
    Json build_proposal(const std::string& task, const std::string& action) const;
    // Caller holds runner locks and has independently validated the full proposal.
    // Approval atomically creates its one execution task and prepared action.
    // decision_created is false on an idempotent repeat: never dispatch it again.
    Json decide_build_proposal(const std::string& task, const std::string& action,
                              const std::string& digest, bool approve,
                              const std::string& expected_workspace);
    // Read a bounded result projected from the immutable proposal, decision and
    // execution receipt. pending/blocked never authorize another execution.
    Json proposal_continuation(const std::string& task, const std::string& action) const;
    // Caller holds runner locks. Atomically append exactly this ready result to
    // the current proposal call and record consumption. An idempotent repeat
    // returns false and does not replace a checkpoint that has since advanced.
    bool consume_build_proposal(const std::string& task, const std::string& action,
                               const Json& expected_result, const Json& next_checkpoint);
    Json recover(); // Caller holds RunLock; no tool execution or automatic retries.
private:
    sqlite3* db_ = nullptr;
};
}
