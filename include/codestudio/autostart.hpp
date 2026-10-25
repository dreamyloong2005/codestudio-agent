#pragma once

#include "codestudio/types.hpp"
#include <string_view>

namespace codestudio {
struct AutostartOptions {
    int poll_ms = 100;
    bool supervise = false;
    int max_restarts = 3;
    int restart_delay_ms = 1000;
};
void validate_autostart_options(const AutostartOptions& options);
// Explicit current-user logon registration. Never starts a worker immediately.
// status is read-only; enable is idempotent only for an identical registration;
// disable removes only a registration whose complete execution contract matches.
Json scheduler_autostart(const std::filesystem::path& workspace,
                         const std::filesystem::path& database,
                         std::string_view action, const AutostartOptions& options = {});

namespace autostart_detail {
// The immutable native definition is also the authorization comparison surface.
// These pure helpers perform no registration, process launch or filesystem IO.
Json plan(const std::filesystem::path& executable, const std::filesystem::path& workspace,
          const std::filesystem::path& database, const std::string& sid, const AutostartOptions& options);
// status/removal accept either fully validated supported mode when false;
// enabling requires exact mode, poll interval and supervision limits.
bool matches(const Json& expected, const Json& observed, bool check_options = true);
}
}
