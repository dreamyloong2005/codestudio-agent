#include "codestudio/scheduler.hpp"
#include "codestudio/state_reader.hpp"
#include "codestudio/process_observation.hpp"

namespace codestudio {
namespace {
Json diagnostics(const Json& snapshot) {
    const auto& record=snapshot.at("supervisor");
    const auto& worker=snapshot.at("worker");
    std::string observation="not_checked";
    bool matches=false;
    if (!record.is_null()) {
        observation=observe_process_identity(record.at("pid").get<std::int64_t>(),
            record.at("process_identity").get<std::string>());
        matches=record.at("owner")==worker.at("owner") && record.at("generation")==worker.at("generation");
    }
    return {{"record",record},{"worker_matches",matches},
        {"observation",{{"state",observation},{"observed_at_ms",scheduler_now_ms()}}}};
}
}
Json scheduler_status(StateReader& reader) {
    const auto snapshot=reader.scheduler_snapshot();
    auto result=snapshot.at("worker");
    result["supervisor"]=diagnostics(snapshot);
    return result;
}
Json supervisor_status(StateReader& reader) { return diagnostics(reader.scheduler_snapshot()); }
}
