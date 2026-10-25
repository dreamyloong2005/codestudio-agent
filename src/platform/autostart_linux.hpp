#pragma once
#include "codestudio/autostart.hpp"

namespace codestudio {
Json linux_autostart(const std::filesystem::path& workspace, const std::filesystem::path& database,
                     std::string_view action, const AutostartOptions& options);
Json linux_autostart_plan(const std::filesystem::path& executable, const std::filesystem::path& workspace,
                         const std::filesystem::path& database, const std::string& uid, const AutostartOptions& options);
bool linux_autostart_matches(const Json& expected, const Json& observed, bool check_options);
}
