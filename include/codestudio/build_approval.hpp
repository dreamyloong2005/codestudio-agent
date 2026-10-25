#pragma once
#include "codestudio/application.hpp"
#include "codestudio/store.hpp"

namespace codestudio {
// Application entry points for a trusted caller. Neither operation is a model
// tool. Reviews use a readonly snapshot; decisions require both runner locks.
Json review_build_proposal(const Json& record, const std::filesystem::path& workspace);
CommandResult decide_build_proposal(Store& store,
    const std::filesystem::path& workspace, const std::filesystem::path& database,
    const std::string& task, const std::string& action, const std::string& digest,
    bool approve, const std::atomic_bool& cancelled);
}
