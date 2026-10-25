#pragma once
#include "codestudio/application.hpp"

namespace codestudio {
CommandResult extension_command(const std::vector<std::string>&, const std::atomic_bool&,
                                const CommandCallbacks&, const std::function<void(const std::string&)>& remember);
}
