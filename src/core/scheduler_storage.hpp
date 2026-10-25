#pragma once

#include "../provider/protocol.hpp"
#include <cstdint>
#include <stdexcept>

namespace codestudio::scheduler_storage {
inline constexpr auto columns = "id,task_id,status,resume_argv,binding,budget_turns,expected_revision,due_at_ms,deadline_at_ms,max_attempts,retry_delay_ms,attempts,owner,generation,claim_token,cancel_requested,last_error,created_at_ms,updated_at_ms,rowid";
inline void identifier(const std::string& value) {
    if (value.empty() || value.size()>256 || value.find('\0')!=std::string::npos)
        throw std::invalid_argument("Scheduler identifier must contain 1..256 bytes without NUL");
}
template<class Statement> Json job(const Statement& row) {
    for (int column : {0,1,2,12,14})
        if (row.bytes(column)>256) throw std::runtime_error("Scheduler metadata exceeds read limit");
    if (row.bytes(3)>65536 || row.bytes(4)>262144 || row.bytes(16)>4096)
        throw std::runtime_error("Scheduler record exceeds read limit");
    auto argv = protocol::strict_json(row.text(3));
    auto binding = protocol::strict_json(row.text(4));
    if (!argv.is_array() || !binding.is_object() || binding.value("version",Json())!=2 ||
        !binding.at("version").is_number_integer())
        throw std::runtime_error("Invalid scheduler record format");
    for (const auto& arg : argv)
        if (!arg.is_string()) throw std::runtime_error("Invalid scheduler arguments");
    return {{"id",row.text(0)},{"task_id",row.text(1)},{"status",row.text(2)},
        {"resume_argv",std::move(argv)},{"binding",std::move(binding)},
        {"budget_turns",row.number(5)},{"expected_revision",row.number(6)},
        {"due_at_ms",row.number(7)},{"deadline_at_ms",row.number(8)},
        {"max_attempts",row.number(9)},{"retry_delay_ms",row.number(10)},
        {"attempts",row.number(11)},{"owner",row.text(12)},{"generation",row.number(13)},
        {"claim_token",row.text(14)},{"cancel_requested",row.number(15)!=0},
        {"last_error",row.text(16)},{"created_at_ms",row.number(17)},{"updated_at_ms",row.number(18)}};
}
inline Json empty_control() {
    return {{"state","stopped"},{"owner",""},{"pid",0},{"generation",0},{"stop_epoch",0},
        {"started_at_ms",0},{"heartbeat_at_ms",0}};
}
template<class Statement> Json control(const Statement& row) {
    if (row.bytes(0)>256 || row.bytes(1)>256) throw std::runtime_error("Scheduler control exceeds read limit");
    return {{"state",row.text(0)},{"owner",row.text(1)},{"pid",row.number(2)},
        {"generation",row.number(3)},{"stop_epoch",row.number(4)},
        {"started_at_ms",row.number(5)},{"heartbeat_at_ms",row.number(6)}};
}
}
