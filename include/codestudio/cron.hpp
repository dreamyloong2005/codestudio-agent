#pragma once

#include <cstdint>
#include <string>

namespace codestudio {
// Resolve "local" once when enrolling a schedule, then persist the returned
// IANA name. "UTC" works without consulting the platform time-zone database.
std::string cron_timezone(const std::string& zone);

// Five numeric fields: minute, hour, day-of-month, month, day-of-week.
// Supports *, lists, inclusive ranges and positive steps. Sunday is 0 or 7.
// Restricted day fields are ORed; an unrestricted * field leaves the other
// day field in control. Impossible calendar dates and unknown zones throw.
void validate_cron(const std::string& expression, const std::string& zone);

// Return UTC epoch milliseconds at a scheduled local minute boundary.
// next is strictly after its argument; previous is at or before it.
// DST gaps are skipped and both instants of a repeated minute are eligible.
// Searches are bounded to eight years; supported UTC years are 1601..9999.
std::int64_t cron_next(const std::string& expression, const std::string& zone,
                       std::int64_t after_ms);
std::int64_t cron_previous(const std::string& expression, const std::string& zone,
                           std::int64_t at_ms);
}
