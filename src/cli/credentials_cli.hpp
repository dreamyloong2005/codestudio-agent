#pragma once
#include "codestudio/application.hpp"
#include <optional>

namespace codestudio {
// CLI-only secret input. The task-service stream is never used as a secret pipe.
std::optional<CommandResult> credential_cli(const std::vector<std::string>& args,
                                           const std::atomic_bool& cancelled);
}
