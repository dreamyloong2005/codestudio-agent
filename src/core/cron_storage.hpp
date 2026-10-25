#pragma once

#include "../provider/protocol.hpp"
#include "scheduler_storage.hpp"
#include <cstdint>
#include <stdexcept>

namespace codestudio::cron_storage {
inline constexpr auto columns = "id,name,expression,timezone,status,workspace,prompt,provider,resume_argv,binding,deadline_ms,max_attempts,retry_delay_ms,max_runs,run_count,next_due_at_ms,last_due_at_ms,last_job_id,last_error,created_at_ms,updated_at_ms,rowid";
inline constexpr int cursor_column = 21;

template<class Statement> Json definition(const Statement& row) {
    for (int column : {0,1,3,4,7,17})
        if (row.bytes(column)>256) throw std::runtime_error("Cron metadata exceeds read limit");
    if (row.bytes(2)>512 || row.bytes(5)>32768 || row.bytes(6)>65536 ||
        row.bytes(8)>65536 || row.bytes(9)>262144 || row.bytes(18)>4096)
        throw std::runtime_error("Cron record exceeds read limit");
    auto argv=protocol::strict_json(row.text(8));
    auto binding=protocol::strict_json(row.text(9));
    if (!argv.is_array() || !binding.is_object() || binding.value("version",Json())!=2 ||
        !binding.at("version").is_number_integer()) throw std::runtime_error("Invalid cron record format");
    for (const auto& argument : argv)
        if (!argument.is_string()) throw std::runtime_error("Invalid cron argument format");
    return {{"id",row.text(0)},{"name",row.text(1)},{"expression",row.text(2)},
        {"timezone",row.text(3)},{"status",row.text(4)},{"workspace",row.text(5)},
        {"prompt",row.text(6)},{"provider",row.text(7)},{"resume_argv",std::move(argv)},
        {"binding",std::move(binding)},{"deadline_ms",row.number(10)},
        {"max_attempts",row.number(11)},{"retry_delay_ms",row.number(12)},
        {"max_runs",row.number(13)},{"run_count",row.number(14)},
        {"next_due_at_ms",row.number(15)},{"last_due_at_ms",row.number(16)},
        {"last_job_id",row.text(17)},{"last_error",row.text(18)},
        {"created_at_ms",row.number(19)},{"updated_at_ms",row.number(20)}};
}
}
