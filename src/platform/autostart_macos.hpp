#pragma once
#include "codestudio/autostart.hpp"

namespace codestudio {
Json macos_autostart(const std::filesystem::path& workspace, const std::filesystem::path& database,
                    std::string_view action, const AutostartOptions& options);
Json macos_autostart_plan(const std::filesystem::path& executable, const std::filesystem::path& workspace,
                         const std::filesystem::path& database, const std::string& uid, const AutostartOptions& options);
// Strict parser for bounded launchctl print-disabled output; throws for unknown syntax.
bool macos_launchd_disabled_override(std::string_view output, const std::string& label);
bool macos_autostart_matches(const Json& expected, const Json& observed, bool check_options);
}
