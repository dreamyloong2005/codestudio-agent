#pragma once
#include "codestudio/application.hpp"

namespace codestudio {
class StateReader;
std::int64_t scheduler_now_ms();
// Historical state and a separate read-only OS identity observation. Neither
// the returned PID nor the observation grants authority to control a process.
Json scheduler_status(StateReader& reader);
Json supervisor_status(StateReader& reader);
// Worker lifecycle and queue controls. Called before ordinary execution locks.
CommandResult scheduler_command(const std::vector<std::string>& argv,
                                const std::atomic_bool& cancelled);
}
