#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace codestudio {
// Launch only this executable's scheduler-run or scheduler-supervise command. argv includes a program
// placeholder, replaced with the current module's absolute executable path.
// Required options: absolute --workspace/--state, --owner, --poll-ms (20..5000).
// Supervision also requires max-restarts, restart-delay-ms and both expected
// stop-epoch/generation fences; command construction never accepts shell text.
// The worker inherits the current environment, but only null standard handles;
// no client pipes, runner locks or parent-owned process lifetime are inherited.
// Windows verifies escape from any parent Job before allowing the worker to run.
// Linux detaches through setsid/double-fork and confirms successful exec over a pipe.
// The returned PID is informational, not authority to terminate a later process.
// The caller confirms its owner token in persistent state before reporting ready.
// Other platforms reject background launch; foreground scheduler-run is separate.
std::uint32_t start_scheduler_process(const std::vector<std::string>& argv);
}
