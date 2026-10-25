#pragma once

#include "codestudio/store.hpp"
#include "scheduler_storage.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace codestudio::supervisor_storage {
inline constexpr auto columns = "id,owner,workspace,process_identity,pid,poll_ms,max_restarts,restart_delay_ms,phase,restarts,worker_pid,generation,stop_epoch,next_restart_at_ms,last_exit_code,exit_reason,started_at_ms,updated_at_ms,heartbeat_at_ms,finished_at_ms";

inline bool reason(const std::string& value) {
    constexpr std::array values{"normal_exit","stop_requested","cancelled","restart_limit",
        "startup_failed","ownership_changed","internal_error","superseded"};
    return std::ranges::find(values,value)!=values.end();
}
inline void options(const SupervisorRunOptions& value) {
    scheduler_storage::identifier(value.id);
    scheduler_storage::identifier(value.owner);
    scheduler_storage::identifier(value.process_identity);
    if (value.workspace.empty() || value.workspace.size()>32768 || value.workspace.find('\0')!=std::string::npos)
        throw std::invalid_argument("Supervisor workspace must contain 1..32768 bytes without NUL");
    if (value.pid<=0 || value.poll_ms<20 || value.poll_ms>5000 || value.max_restarts<0 ||
        value.max_restarts>100 || value.restart_delay_ms<100 || value.restart_delay_ms>60000)
        throw std::invalid_argument("Invalid supervisor process or restart options");
}
inline void state(const SupervisorRunState& value, int max_restarts) {
    constexpr std::array phases{"starting","running","backoff","stopping","stopped"};
    if (std::ranges::find(phases,value.phase)==phases.end() || value.restarts<0 || value.restarts>max_restarts ||
        value.worker_pid<0 || value.generation<0 || value.next_restart_at_ms<0 ||
        (value.last_exit_code && *value.last_exit_code<0))
        throw std::invalid_argument("Invalid supervisor lifecycle state");
    if (value.phase=="backoff") {
        if (value.worker_pid!=0 || value.next_restart_at_ms<=0 || !value.last_exit_code)
            throw std::invalid_argument("Supervisor backoff requires a recorded child exit and due time");
    } else if (value.next_restart_at_ms!=0)
        throw std::invalid_argument("Supervisor restart time requires backoff");
    if (value.phase=="running" && (value.generation<=0 || (value.worker_pid==0 && !value.last_exit_code)))
        throw std::invalid_argument("Supervisor running state requires a registered child");
    if (value.phase=="stopped") {
        if (value.worker_pid!=0 || !reason(value.exit_reason))
            throw std::invalid_argument("Stopped supervisor requires an exit reason and no child");
    } else if (!value.exit_reason.empty() && (value.phase!="stopping" || !reason(value.exit_reason)))
        throw std::invalid_argument("Supervisor exit reason requires stopping or stopped state");
}
inline SupervisorRunState saved_state(const Json& value) {
    SupervisorRunState result{value.at("phase").get<std::string>(),value.at("restarts").get<int>(),
        value.at("worker_pid").get<std::int64_t>(),value.at("generation").get<std::int64_t>(),
        value.at("next_restart_at_ms").get<std::int64_t>(),std::nullopt,value.at("exit_reason").get<std::string>()};
    if (!value.at("last_exit_code").is_null()) result.last_exit_code=value.at("last_exit_code").get<std::int64_t>();
    return result;
}
inline void record(const Json& value) {
    if (!value.is_object() || value.size()!=20) throw std::runtime_error("Invalid supervisor record envelope");
    for (const auto* name : {"id","owner","workspace","process_identity","phase","exit_reason"})
        if (!value.contains(name) || !value.at(name).is_string()) throw std::runtime_error("Invalid supervisor text metadata");
    for (const auto* name : {"pid","poll_ms","max_restarts","restart_delay_ms","restarts","worker_pid","generation",
                            "stop_epoch","next_restart_at_ms","started_at_ms","updated_at_ms","heartbeat_at_ms","finished_at_ms"}) {
        if (!value.contains(name) || !value.at(name).is_number_integer() ||
            (value.at(name).is_number_unsigned() && value.at(name).get<std::uint64_t>()>static_cast<std::uint64_t>(INT64_MAX)))
            throw std::runtime_error("Invalid supervisor integer metadata");
    }
    if (!value.contains("last_exit_code") || (!value.at("last_exit_code").is_null() &&
        (!value.at("last_exit_code").is_number_integer() || (value.at("last_exit_code").is_number_unsigned() &&
         value.at("last_exit_code").get<std::uint64_t>()>static_cast<std::uint64_t>(INT64_MAX)))))
        throw std::runtime_error("Invalid supervisor exit code");
    if (value.at("poll_ms")<20 || value.at("poll_ms")>5000 || value.at("max_restarts")<0 || value.at("max_restarts")>100 ||
        value.at("restart_delay_ms")<100 || value.at("restart_delay_ms")>60000 || value.at("restarts")<0 || value.at("restarts")>100)
        throw std::runtime_error("Invalid supervisor option bounds");
    options({value.at("id").get<std::string>(),value.at("owner").get<std::string>(),value.at("workspace").get<std::string>(),
        value.at("process_identity").get<std::string>(),value.at("pid").get<std::int64_t>(),value.at("poll_ms").get<int>(),
        value.at("max_restarts").get<int>(),value.at("restart_delay_ms").get<int>()});
    state(saved_state(value),value.at("max_restarts").get<int>());
    if (value.at("stop_epoch")<0 || value.at("started_at_ms")<=0 || value.at("updated_at_ms")<value.at("started_at_ms") ||
        value.at("heartbeat_at_ms")<value.at("started_at_ms") || value.at("heartbeat_at_ms")>value.at("updated_at_ms") ||
        (value.at("phase")=="stopped" ? value.at("finished_at_ms")!=value.at("updated_at_ms") : value.at("finished_at_ms")!=0))
        throw std::runtime_error("Invalid supervisor lifecycle timestamps");
}
inline void transition(const Json& previous, const SupervisorRunState& next, const std::string& event_kind) {
    const auto old=saved_state(previous);
    state(next,previous.at("max_restarts").get<int>());
    if (next.generation<old.generation || next.generation-old.generation>1 ||
        next.restarts<old.restarts || next.restarts>old.restarts+1)
        throw std::invalid_argument("Supervisor generation and restart budget cannot regress or skip");
    if (next.restarts!=old.restarts && !(old.phase=="backoff" && next.phase=="starting"))
        throw std::invalid_argument("Supervisor restart budget advances only when leaving backoff");
    if (event_kind=="supervisor.child_exited") {
        if (next.phase!=old.phase || old.worker_pid<=0 || next.worker_pid!=0 || !next.last_exit_code ||
            next.restarts!=old.restarts || next.next_restart_at_ms!=old.next_restart_at_ms ||
            next.exit_reason!=old.exit_reason)
            throw std::invalid_argument("Supervisor child exit requires its recorded child and exit code");
        return;
    }
    if (event_kind!="supervisor."+next.phase)
        throw std::invalid_argument("Supervisor event kind must match lifecycle state");
    const bool allowed = (old.phase=="starting" && (next.phase=="starting" || next.phase=="running" || next.phase=="stopping" || next.phase=="stopped")) ||
        (old.phase=="running" && (next.phase=="backoff" || next.phase=="stopping" || next.phase=="stopped")) ||
        (old.phase=="backoff" && (next.phase=="starting" || next.phase=="stopping" || next.phase=="stopped")) ||
        (old.phase=="stopping" && (next.phase=="stopping" || next.phase=="stopped"));
    if (!allowed || (old.phase=="backoff" && next.phase=="starting" && next.restarts!=old.restarts+1))
        throw std::invalid_argument("Invalid supervisor phase transition");
    if (next.last_exit_code!=old.last_exit_code)
        throw std::invalid_argument("Only a child exit event may change its exit code");
}
template<class Statement> Json run(const Statement& row) {
    for (int column : {0,1,3,8,15})
        if (row.bytes(column)>256) throw std::runtime_error("Supervisor metadata exceeds read limit");
    if (row.bytes(2)>32768) throw std::runtime_error("Supervisor workspace exceeds read limit");
    for (int column : {4,5,6,7,9,10,11,12,13,16,17,18,19})
        if (!row.integer(column)) throw std::runtime_error("Invalid supervisor integer metadata");
    if (!row.null(14) && !row.integer(14)) throw std::runtime_error("Invalid supervisor exit code");
    Json result{{"id",row.text(0)},{"owner",row.text(1)},{"workspace",row.text(2)},
        {"process_identity",row.text(3)},{"pid",row.number(4)},{"poll_ms",row.number(5)},
        {"max_restarts",row.number(6)},{"restart_delay_ms",row.number(7)},{"phase",row.text(8)},
        {"restarts",row.number(9)},{"worker_pid",row.number(10)},{"generation",row.number(11)},
        {"stop_epoch",row.number(12)},{"next_restart_at_ms",row.number(13)},
        {"last_exit_code",row.null(14) ? Json(nullptr) : Json(row.number(14))},{"exit_reason",row.text(15)},
        {"started_at_ms",row.number(16)},{"updated_at_ms",row.number(17)},
        {"heartbeat_at_ms",row.number(18)},{"finished_at_ms",row.number(19)}};
    record(result);
    return result;
}
}
