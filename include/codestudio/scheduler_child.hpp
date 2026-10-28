#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace codestudio {
struct SchedulerChildOptions {
    std::filesystem::path workspace;
    std::filesystem::path database;
    std::string owner;
    int poll_ms = 250;
    std::int64_t expected_stop_epoch = 0;
    std::int64_t expected_generation = 0;
};

// Owns one scheduler worker, launched from this executable with fixed arguments.
// Destruction terminates and reaps an active child; normal shutdown should first
// persist the stop epoch and poll until the worker exits. The PID is informational.
// Methods require one owner thread; no other code may reap the owned POSIX child.
// On Linux, that thread must outlive the child because PDEATHSIG tracks its lifetime.
class SchedulerChild {
public:
    virtual ~SchedulerChild() = default;
    virtual std::uint32_t pid() const = 0;
    // Nonblocking. An observed exit code remains available on subsequent calls.
    // POSIX signal termination is represented as 128 + signal number.
    virtual std::optional<std::int64_t> poll_exit() = 0;
};

// Inherits the environment (including provider credentials), but only null
// standard streams. No arbitrary executable or command arguments are accepted.
// Windows retains an owned kill-on-close Job; Linux uses a direct child with
// parent-death termination and confirms exec over a close-on-exec pipe. macOS
// uses posix_spawn with a worker-side, birth-identity-checked kqueue parent watch.
std::unique_ptr<SchedulerChild> start_scheduler_child(const SchedulerChildOptions& options);
#ifdef __APPLE__
// Install before opening worker state or accepting work. The watch binds to the
// actual parent process; its exit terminates this worker's own process group.
// The worker must have its own group, as established by start_scheduler_child.
class SchedulerParentGuard {
public:
    virtual ~SchedulerParentGuard() = default;
};
std::unique_ptr<SchedulerParentGuard> watch_scheduler_parent(std::uint32_t pid, const std::string& identity);
#endif
}
