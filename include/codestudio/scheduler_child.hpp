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
// Methods require one owner thread; no other code may reap the owned Linux child.
// On Linux, that thread must outlive the child because PDEATHSIG tracks its lifetime.
class SchedulerChild {
public:
    virtual ~SchedulerChild() = default;
    virtual std::uint32_t pid() const = 0;
    // Nonblocking. An observed exit code remains available on subsequent calls.
    // Linux signal termination is represented as 128 + signal number.
    virtual std::optional<std::int64_t> poll_exit() = 0;
};

// Inherits the environment (including provider credentials), but only null
// standard streams. No arbitrary executable or command arguments are accepted.
// Windows retains an owned kill-on-close Job; Linux uses a direct child with
// parent-death termination and confirms exec over a close-on-exec pipe.
std::unique_ptr<SchedulerChild> start_scheduler_child(const SchedulerChildOptions& options);
}
