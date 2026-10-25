#pragma once

#include "codestudio/types.hpp"
#include <cstdint>
#include <memory>

namespace codestudio {
// Read-only, committed views for local clients. Own a separate reader per
// caller thread; no writer connection or execution lock is shared with it.
// Missing or uninitialized (schema 0) state produces empty pages until a writer
// commits initialization. Neither condition creates or migrates state here.
// Initialized databases must use the current schema 7; no legacy format is read.
class StateReader {
public:
    explicit StateReader(const std::filesystem::path& database);
    ~StateReader();
    StateReader(const StateReader&) = delete;
    StateReader& operator=(const StateReader&) = delete;

    // Cursor 0 means the newest task; rows are ordered by descending rowid.
    Json tasks(std::int64_t before = 0, std::size_t limit = 50) const;
    Json task(const std::string& id) const;
    // Database-wide committed event sequence, in ascending order.
    Json events(std::int64_t after = 0, std::size_t limit = 50) const;
    // Exact proposal receipt and committed decision, without creating/migrating
    // state. Oversized receipt data is rejected, never silently truncated.
    Json build_proposal(const std::string& task, const std::string& action) const;
    Json schedules(std::int64_t before = 0, std::size_t limit = 50) const;
    Json schedule(const std::string& id) const;
    Json scheduler() const;
    // Latest recorded lifecycle, not a claim that the process is still alive.
    Json supervisor() const;
    Json scheduler_snapshot() const;
    // Independent database-wide sequence; task event cursors are not compatible.
    Json supervisor_events(std::int64_t after = 0, std::size_t limit = 100) const;
    Json crons(std::int64_t before = 0, std::size_t limit = 50) const;
    Json cron(const std::string& id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
