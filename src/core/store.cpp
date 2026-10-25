#include "codestudio/store.hpp"
#include "codestudio/digest.hpp"
#include "codestudio/cron.hpp"
#include "../provider/protocol.hpp"
#include "scheduler_storage.hpp"
#include "cron_storage.hpp"
#include "supervisor_storage.hpp"
#include <sqlite3.h>
#include <array>
#include <algorithm>
#include <climits>
#include <chrono>
#include <cerrno>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace codestudio {
namespace {
void sql(sqlite3* db, const char* text) {
    char* message = nullptr;
    if (sqlite3_exec(db, text, nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string error = message ? message : "SQLite operation failed";
        sqlite3_free(message);
        throw std::runtime_error(error);
    }
}
class Statement {
public:
    Statement(sqlite3* db, const char* text) : db_(db) {
        if (sqlite3_prepare_v2(db, text, -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    void bind(int index, const std::string& value) {
        if (value.size() > static_cast<std::size_t>(INT_MAX) ||
            sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("SQLite bind failed");
    }
    void bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_,index,value) != SQLITE_OK)
            throw std::runtime_error("SQLite integer bind failed");
    }
    void bind_null(int index) {
        if (sqlite3_bind_null(stmt_,index)!=SQLITE_OK) throw std::runtime_error("SQLite null bind failed");
    }
    bool next() {
        const auto result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    std::string text(int column) const {
        const auto* value = sqlite3_column_text(stmt_, column);
        return value ? std::string(reinterpret_cast<const char*>(value),
                                   sqlite3_column_bytes(stmt_, column)) : "";
    }
    long long number(int column) const { return sqlite3_column_int64(stmt_, column); }
    bool integer(int column) const { return sqlite3_column_type(stmt_,column)==SQLITE_INTEGER; }
    bool null(int column) const { return sqlite3_column_type(stmt_,column)==SQLITE_NULL; }
    std::size_t bytes(int column) const { return static_cast<std::size_t>(sqlite3_column_bytes(stmt_, column)); }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { sql(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { sql(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};
class ReadSnapshot {
public:
    explicit ReadSnapshot(sqlite3* db) : db_(sqlite3_get_autocommit(db) ? db : nullptr) {
        if (db_) sql(db_, "BEGIN");
    }
    ~ReadSnapshot() { if (db_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
private:
    sqlite3* db_;
};
Json task_row(const Statement& query) {
    return {{"id", query.text(0)}, {"prompt", query.text(1)}, {"workspace", query.text(2)},
            {"provider", query.text(3)}, {"status", query.text(4)}, {"summary", query.text(5)},
            {"created_at", query.text(6)}, {"updated_at", query.text(7)}};
}
void require_changed(sqlite3* db) {
    if (sqlite3_changes(db) != 1) throw std::runtime_error("Invalid state transition or unknown record");
}
constexpr std::size_t checkpoint_limit = 16 * 1024 * 1024;

void validate_checkpoint(const Json& state, const Json& binding) {
    if (!state.is_object() || !state.contains("version") || !state.at("version").is_number_integer() ||
        state.at("version") != 2 || !state.contains("phase") || !state.at("phase").is_string() ||
        !state.contains("resumable") || !state.at("resumable").is_boolean())
        throw std::runtime_error("Invalid task checkpoint state");
    const std::array phases{"ready", "model_pending", "tools", "finished"};
    const auto& phase = state.at("phase").get_ref<const std::string&>();
    if (std::ranges::find(phases, phase) == phases.end())
        throw std::runtime_error("Invalid task checkpoint phase");
    if (!binding.is_object() || !binding.contains("version") ||
        !binding.at("version").is_number_integer() || binding.at("version") != 2)
        throw std::runtime_error("Unsupported task checkpoint binding version");
    if (Json{{"version",2},{"state",state},{"binding",binding}}.dump().size() > checkpoint_limit)
        throw std::runtime_error("Task checkpoint exceeds 16 MiB limit");
}

Json read_checkpoint(sqlite3* db, const std::string& task) {
    Statement query(db, R"(SELECT version,length(CAST(state AS BLOB)),length(CAST(binding AS BLOB)),
        state,binding,updated_at FROM task_checkpoints WHERE task_id=?)");
    query.bind(1,task);
    if (!query.next()) throw std::runtime_error("Task has no checkpoint");
    if (query.text(0) != "2") throw std::runtime_error("Unsupported task checkpoint version");
    if (query.number(1) > static_cast<long long>(checkpoint_limit) ||
        query.number(2) > static_cast<long long>(checkpoint_limit) ||
        query.number(1) + query.number(2) > static_cast<long long>(checkpoint_limit))
        throw std::runtime_error("Task checkpoint exceeds 16 MiB limit");
    const auto state_text = query.text(3);
    const auto binding_text = query.text(4);
    const auto state = protocol::strict_json(state_text);
    const auto binding = protocol::strict_json(binding_text);
    validate_checkpoint(state,binding);
    return {{"version",2},{"state",state},{"binding",binding}};
}

// The caller owns the transaction. Keeping the checkpoint and its associated
// receipt in the same transaction prevents a resumed runner repeating effects.
void save_checkpoint_in_transaction(sqlite3* db, const std::string& task,
                                    const Json& state, const Json* requested_binding) {
    if (requested_binding) validate_checkpoint(state,*requested_binding);
    Statement running(db, "SELECT status FROM tasks WHERE id=?");
    running.bind(1,task);
    if (!running.next() || running.text(0) != "running")
        throw std::runtime_error("Checkpoints can only be saved for running tasks");
    Statement exists(db, "SELECT EXISTS(SELECT 1 FROM task_checkpoints WHERE task_id=?)");
    exists.bind(1,task); exists.next();
    Json binding;
    if (exists.number(0) != 0) {
        binding = read_checkpoint(db,task).at("binding");
        if (requested_binding && binding != *requested_binding)
            throw std::runtime_error("Task checkpoint binding cannot change");
    } else {
        if (!requested_binding) throw std::runtime_error("Task checkpoint binding is missing");
        binding = *requested_binding;
    }
    validate_checkpoint(state,binding);
    Statement update(db, R"(INSERT INTO task_checkpoints(task_id,version,state,binding)
        VALUES(?,2,?,?) ON CONFLICT(task_id) DO UPDATE SET
        state=excluded.state,revision=task_checkpoints.revision+1,
        updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
    update.bind(1,task); update.bind(2,state.dump()); update.bind(3,binding.dump()); update.next();
    Statement schedule(db, R"(UPDATE scheduler_jobs SET expected_revision=
        (SELECT revision FROM task_checkpoints WHERE task_id=?) WHERE task_id=? AND status='running')");
    schedule.bind(1,task); schedule.bind(2,task); schedule.next();
}

bool valid_digest(const Json& value) {
    if (!value.is_string()) return false;
    const auto& digest = value.get_ref<const std::string&>();
    return digest.size() == 64 && std::ranges::all_of(digest, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

const Json& pending_call(const Json& state) {
    if (state.value("phase", "") != "tools" || !state.contains("pending_calls") ||
        !state.at("pending_calls").is_array() || state.at("pending_calls").size() > 16 ||
        !state.contains("next_call") || !state.at("next_call").is_number_integer() ||
        state.at("next_call") < 0 || state.at("next_call") >= state.at("pending_calls").size())
        throw std::runtime_error("Checkpoint does not identify a pending proposal call");
    const auto& call = state.at("pending_calls")[state.at("next_call").get<std::size_t>()];
    if (!call.is_object() || call.value("name", "") != "propose_build" ||
        !call.contains("id") || !call.at("id").is_string() ||
        call.at("id").get_ref<const std::string&>().empty() ||
        call.at("id").get_ref<const std::string&>().size() > 256 ||
        !call.contains("arguments") || !call.at("arguments").is_object())
        throw std::runtime_error("Checkpoint does not identify a pending proposal call");
    return call;
}

std::string pending_proposal(sqlite3* db, const std::string& task, const Json& state) {
    const auto& call = pending_call(state);
    Statement query(db, R"(SELECT id,arguments FROM actions
        WHERE task_id=? AND call_id=? AND tool='propose_build' AND status='succeeded' AND effect='none')");
    query.bind(1,task); query.bind(2,call.at("id").get<std::string>());
    if (!query.next() || query.bytes(0) > 256 || query.bytes(1) > 256 * 1024 ||
        protocol::strict_json(query.text(1)) != call.at("arguments"))
        throw std::runtime_error("Pending proposal does not match its durable receipt");
    return query.text(0);
}

bool proposal_was_consumed(sqlite3* db, const std::string& task, const std::string& action) {
    Statement events(db, "SELECT payload,schema_version FROM events WHERE task_id=? AND kind='build.proposal.consumed' ORDER BY seq");
    events.bind(1,task);
    std::size_t count = 0;
    bool found = false;
    while (events.next()) {
        if (++count > 1600 || events.bytes(0) > 4096 || events.text(1) != "2")
            throw std::runtime_error("Invalid build proposal consumption history");
        const auto payload = protocol::strict_json(events.text(0));
        if (!payload.is_object() || !payload.contains("action_id") || !payload.at("action_id").is_string())
            throw std::runtime_error("Invalid build proposal consumption history");
        if (payload.at("action_id") == action) {
            if (found) throw std::runtime_error("Build proposal has duplicate consumption records");
            found = true;
        }
    }
    return found;
}

std::int64_t current_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
Json read_schedule(sqlite3* db, const std::string& id) {
    scheduler_storage::identifier(id);
    const auto query_text=std::string("SELECT ")+scheduler_storage::columns+" FROM scheduler_jobs WHERE id=?";
    Statement query(db,query_text.c_str()); query.bind(1,id);
    if (!query.next()) throw std::runtime_error("Scheduled job not found");
    return scheduler_storage::job(query);
}
Json read_cron(sqlite3* db, const std::string& id) {
    scheduler_storage::identifier(id);
    const auto text=std::string("SELECT ")+cron_storage::columns+" FROM cron_schedules WHERE id=?";
    Statement query(db,text.c_str()); query.bind(1,id);
    if (!query.next()) throw std::runtime_error("Cron schedule not found");
    return cron_storage::definition(query);
}
void bounded_cron_text(const std::string& value,std::size_t maximum,const char* field) {
    if (value.empty() || value.size()>maximum || value.find('\0')!=std::string::npos)
        throw std::invalid_argument(std::string("Invalid cron ")+field);
}
bool active_schedule(const Json& job) {
    return job.at("status")=="queued" || job.at("status")=="claimed" ||
        job.at("status")=="running" || job.at("status")=="waiting_approval";
}
std::int64_t revision(sqlite3* db, const std::string& task) {
    Statement query(db,"SELECT revision FROM task_checkpoints WHERE task_id=?"); query.bind(1,task);
    if (!query.next() || query.number(0)<=0) throw std::runtime_error("Task has no valid checkpoint revision");
    return query.number(0);
}
std::string scheduler_reason(const std::string& reason) {
    auto length=std::min<std::size_t>(reason.size(),4096);
    while (length<reason.size() && length>0 && (static_cast<unsigned char>(reason[length])&0xc0)==0x80) --length;
    return reason.substr(0,length);
}
void advance_cron_after_task(sqlite3* db,const std::string& task,std::int64_t now) {
    // The worker cannot scan cron while a runner owns the workspace. Persist
    // the end of this instance's active lifetime in the same transaction as
    // its terminal outcome, so those slots cannot become catch-up work later.
    Statement latest(db,R"(SELECT c.id,c.expression,c.timezone,c.next_due_at_ms,c.last_due_at_ms
        FROM cron_schedules c JOIN scheduler_jobs j ON j.id=c.last_job_id
        WHERE j.task_id=? AND c.status='active')");
    latest.bind(1,task);
    if (!latest.next()) return;
    const auto next=cron_next(latest.text(1),latest.text(2),std::max(now,static_cast<std::int64_t>(latest.number(4))));
    if (next<=latest.number(3)) return;
    Statement advance(db,"UPDATE cron_schedules SET next_due_at_ms=?,updated_at_ms=? WHERE id=? AND status='active'");
    advance.bind(1,next); advance.bind(2,now); advance.bind(3,latest.text(0)); advance.next(); require_changed(db);
}
void scheduler_transition(sqlite3* db, const Json& job, const std::string& status,
                          const std::string& reason, std::int64_t now, std::int64_t due=0) {
    Statement update(db,R"(UPDATE scheduler_jobs SET status=?,last_error=?,updated_at_ms=?,due_at_ms=?,
        claim_token='' WHERE id=? AND claim_token=? AND status=?)");
    update.bind(1,status); update.bind(2,scheduler_reason(reason)); update.bind(3,now);
    update.bind(4,due ? due : job.at("due_at_ms").get<std::int64_t>());
    update.bind(5,job.at("id").get<std::string>()); update.bind(6,job.at("claim_token").get<std::string>());
    update.bind(7,job.at("status").get<std::string>()); update.next(); require_changed(db);
    Statement event(db,"INSERT INTO events(task_id,kind,payload) VALUES(?,?,?)");
    event.bind(1,job.at("task_id").get<std::string>()); event.bind(2,"scheduler."+status);
    event.bind(3,Json{{"job_id",job.at("id")},{"previous_status",job.at("status")},
        {"status",status},{"reason",scheduler_reason(reason)},{"attempts",job.at("attempts")}}.dump()); event.next();
    if (status=="completed" || status=="cancelled" || status=="expired" || status=="exhausted" || status=="blocked")
        advance_cron_after_task(db,job.at("task_id").get<std::string>(),now);
}
bool uncertain_actions(sqlite3* db, const std::string& task) {
    Statement query(db,R"(SELECT EXISTS(SELECT 1 FROM actions WHERE task_id=? AND
        (status IN ('prepared','dispatched','unknown') OR effect IN ('partial','unknown'))))");
    query.bind(1,task); query.next(); return query.number(0)!=0;
}
bool remaining_budget(const Json& state, const Json& job) {
    return state.contains("turns") && state.at("turns").is_number_integer() && state.at("turns")>=0 &&
        state.contains("max_turns") && state.at("max_turns").is_number_integer() &&
        state.at("max_turns")==job.at("budget_turns") && state.at("turns")<job.at("budget_turns");
}
std::int64_t retry_at(const Json& job, std::int64_t now) {
    auto delay=job.at("retry_delay_ms").get<std::int64_t>();
    const auto attempt=job.at("attempts").get<int>();
    for (int i=1;i<attempt && delay<60000;++i) delay=std::min<std::int64_t>(60000,delay*2);
    return now+delay;
}
bool exhausted_schedule(sqlite3* db, const Json& job, std::int64_t now) {
    if (job.at("cancel_requested")==true) {
        scheduler_transition(db,job,"cancelled","Scheduled job cancelled",now); return true;
    }
    if (now>=job.at("deadline_at_ms").get<std::int64_t>()) {
        scheduler_transition(db,job,"expired","Scheduled job deadline reached",now); return true;
    }
    if (job.at("attempts")>=job.at("max_attempts")) {
        scheduler_transition(db,job,"exhausted","Scheduled job attempt limit reached",now); return true;
    }
    return false;
}
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::filesystem::path utf8_path(const std::string& text) {
    if (text.find('\0') != std::string::npos) throw std::runtime_error("Path contains NUL");
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string new_id() {
    std::random_device random;
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) result << std::setw(8) << static_cast<std::uint32_t>(random());
    return result.str();
}

struct RunLock::Impl {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};
RunLock::RunLock(const std::filesystem::path& database) : impl_(std::make_unique<Impl>()) {
    const auto absolute = std::filesystem::absolute(database);
    std::filesystem::create_directories(absolute.parent_path());
    auto path = absolute;
    path += ".run.lock";
#ifdef _WIN32
    impl_->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
            throw RunBusy("State database is in use by another runner");
        throw std::runtime_error("Cannot open state database run lock (Windows error " + std::to_string(error) + ")");
    }
#else
    impl_->fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl_->fd < 0) throw std::runtime_error("Cannot open state database run lock");
    if (flock(impl_->fd, LOCK_EX | LOCK_NB) != 0) {
        const auto error = errno;
        close(impl_->fd); impl_->fd = -1;
        if (error == EWOULDBLOCK || error == EAGAIN)
            throw RunBusy("State database is in use by another runner");
        throw std::runtime_error("Cannot acquire state database run lock (errno " + std::to_string(error) + ")");
    }
#endif
}
RunLock::~RunLock() {
#ifdef _WIN32
    if (impl_->handle != INVALID_HANDLE_VALUE) CloseHandle(impl_->handle);
#else
    if (impl_->fd >= 0) { flock(impl_->fd, LOCK_UN); close(impl_->fd); }
#endif
}

Store::Store(const std::filesystem::path& database) {
    const auto absolute = std::filesystem::absolute(database);
    std::filesystem::create_directories(absolute.parent_path());
    if (sqlite3_open_v2(path_utf8(absolute).c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "Cannot open SQLite database";
        sqlite3_close(db_); db_ = nullptr;
        throw std::runtime_error(message);
    }
    try {
        sqlite3_busy_timeout(db_, 5000);
        long long schema_version = 0;
        {
            Statement version(db_, "PRAGMA user_version");
            version.next();
            schema_version = version.number(0);
            if (schema_version != 0 && schema_version != 7)
                throw std::runtime_error("Unsupported state schema; use a new state database");
            if (schema_version == 0) {
                Statement objects(db_, "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'");
                objects.next();
                if (objects.number(0) != 0)
                    throw std::runtime_error("Unversioned state database is not empty; use a new state database");
            }
        }
        sql(db_, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
        if (schema_version == 7) return;
        Transaction tx(db_);
        sql(db_, R"(
            CREATE TABLE tasks (
                id TEXT PRIMARY KEY, prompt TEXT NOT NULL, workspace TEXT NOT NULL,
                provider TEXT NOT NULL, status TEXT NOT NULL, summary TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
            );
            CREATE TABLE events (
                seq INTEGER PRIMARY KEY AUTOINCREMENT, task_id TEXT NOT NULL REFERENCES tasks(id),
                kind TEXT NOT NULL, payload TEXT NOT NULL, schema_version INTEGER NOT NULL DEFAULT 2,
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
            );
            CREATE TABLE actions (
                id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id),
                call_id TEXT NOT NULL, tool TEXT NOT NULL, arguments TEXT NOT NULL,
                status TEXT NOT NULL, effect TEXT NOT NULL DEFAULT 'none', result TEXT NOT NULL DEFAULT '{}',
                UNIQUE(task_id,call_id)
            );
            CREATE INDEX events_task ON events(task_id,seq);
            CREATE TABLE task_checkpoints (
                task_id TEXT PRIMARY KEY REFERENCES tasks(id), version INTEGER NOT NULL,
                state TEXT NOT NULL, binding TEXT NOT NULL, revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),
                updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
            );
            CREATE TABLE build_proposal_decisions (
                source_action_id TEXT PRIMARY KEY REFERENCES actions(id),
                digest TEXT NOT NULL CHECK(length(digest)=64 AND digest NOT GLOB '*[^0-9a-f]*'),
                decision TEXT NOT NULL CHECK(decision IN ('approved','rejected')),
                execution_task_id TEXT UNIQUE REFERENCES tasks(id),
                execution_action_id TEXT UNIQUE REFERENCES actions(id),
                created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
                CHECK((decision='approved' AND execution_task_id IS NOT NULL AND execution_action_id IS NOT NULL)
                   OR (decision='rejected' AND execution_task_id IS NULL AND execution_action_id IS NULL))
            );
            CREATE TABLE scheduler_jobs (
                id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(id),
                status TEXT NOT NULL CHECK(status IN ('queued','claimed','running','waiting_approval','completed','blocked','cancelled','expired','exhausted')),
                resume_argv TEXT NOT NULL, binding TEXT NOT NULL,
                budget_turns INTEGER NOT NULL CHECK(budget_turns BETWEEN 1 AND 100),
                expected_revision INTEGER NOT NULL CHECK(expected_revision>0),
                due_at_ms INTEGER NOT NULL, deadline_at_ms INTEGER NOT NULL,
                max_attempts INTEGER NOT NULL CHECK(max_attempts BETWEEN 1 AND 100),
                retry_delay_ms INTEGER NOT NULL CHECK(retry_delay_ms BETWEEN 100 AND 60000),
                attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts>=0),
                owner TEXT NOT NULL DEFAULT '', generation INTEGER NOT NULL DEFAULT 0,
                claim_token TEXT NOT NULL DEFAULT '', cancel_requested INTEGER NOT NULL DEFAULT 0,
                last_error TEXT NOT NULL DEFAULT '', created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL
            );
            CREATE UNIQUE INDEX scheduler_active_task ON scheduler_jobs(task_id)
                WHERE status IN ('queued','claimed','running','waiting_approval');
            CREATE INDEX scheduler_due ON scheduler_jobs(status,due_at_ms);
            CREATE TABLE scheduler_control (
                singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                generation INTEGER NOT NULL DEFAULT 0, stop_epoch INTEGER NOT NULL DEFAULT 0,
                owner TEXT NOT NULL DEFAULT '', pid INTEGER NOT NULL DEFAULT 0,
                started_at_ms INTEGER NOT NULL DEFAULT 0, heartbeat_at_ms INTEGER NOT NULL DEFAULT 0,
                state TEXT NOT NULL DEFAULT 'stopped' CHECK(state IN ('running','stopped'))
            );
            INSERT INTO scheduler_control(singleton) VALUES(1);
            CREATE TABLE cron_schedules (
                id TEXT PRIMARY KEY, name TEXT NOT NULL, expression TEXT NOT NULL,
                timezone TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('active','paused','cancelled','exhausted')),
                workspace TEXT NOT NULL, prompt TEXT NOT NULL, provider TEXT NOT NULL,
                resume_argv TEXT NOT NULL, initial_state TEXT NOT NULL, binding TEXT NOT NULL,
                deadline_ms INTEGER NOT NULL CHECK(deadline_ms BETWEEN 1 AND 604800000),
                max_attempts INTEGER NOT NULL CHECK(max_attempts BETWEEN 1 AND 100),
                retry_delay_ms INTEGER NOT NULL CHECK(retry_delay_ms BETWEEN 100 AND 60000),
                max_runs INTEGER NOT NULL CHECK(max_runs BETWEEN 0 AND 1000000),
                run_count INTEGER NOT NULL DEFAULT 0 CHECK(run_count>=0),
                next_due_at_ms INTEGER NOT NULL, last_due_at_ms INTEGER NOT NULL DEFAULT 0,
                last_job_id TEXT NOT NULL DEFAULT '', last_error TEXT NOT NULL DEFAULT '',
                created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL
            );
            CREATE INDEX cron_due ON cron_schedules(status,workspace,next_due_at_ms);
            CREATE TABLE cron_occurrences (
                cron_id TEXT NOT NULL REFERENCES cron_schedules(id),
                scheduled_at_ms INTEGER NOT NULL,
                task_id TEXT NOT NULL UNIQUE REFERENCES tasks(id),
                job_id TEXT NOT NULL UNIQUE REFERENCES scheduler_jobs(id),
                PRIMARY KEY(cron_id,scheduled_at_ms)
            );
            CREATE TABLE supervisor_runs (
                id TEXT PRIMARY KEY, owner TEXT NOT NULL, workspace TEXT NOT NULL, process_identity TEXT NOT NULL,
                pid INTEGER NOT NULL CHECK(pid>0), poll_ms INTEGER NOT NULL CHECK(poll_ms BETWEEN 20 AND 5000),
                max_restarts INTEGER NOT NULL CHECK(max_restarts BETWEEN 0 AND 100),
                restart_delay_ms INTEGER NOT NULL CHECK(restart_delay_ms BETWEEN 100 AND 60000),
                phase TEXT NOT NULL CHECK(phase IN ('starting','running','backoff','stopping','stopped')),
                restarts INTEGER NOT NULL DEFAULT 0 CHECK(restarts>=0 AND restarts<=max_restarts),
                worker_pid INTEGER NOT NULL DEFAULT 0 CHECK(worker_pid>=0),
                generation INTEGER NOT NULL CHECK(generation>=0), stop_epoch INTEGER NOT NULL CHECK(stop_epoch>=0),
                next_restart_at_ms INTEGER NOT NULL DEFAULT 0 CHECK(next_restart_at_ms>=0), last_exit_code INTEGER,
                exit_reason TEXT NOT NULL DEFAULT '', started_at_ms INTEGER NOT NULL,
                updated_at_ms INTEGER NOT NULL, heartbeat_at_ms INTEGER NOT NULL, finished_at_ms INTEGER NOT NULL DEFAULT 0
            );
            CREATE UNIQUE INDEX supervisor_active_run ON supervisor_runs((1)) WHERE phase!='stopped';
            CREATE TABLE supervisor_events (
                seq INTEGER PRIMARY KEY AUTOINCREMENT, run_id TEXT NOT NULL REFERENCES supervisor_runs(id),
                kind TEXT NOT NULL, payload TEXT NOT NULL, created_at_ms INTEGER NOT NULL
            );
            CREATE INDEX supervisor_events_run ON supervisor_events(run_id,seq);
            PRAGMA user_version=7;
        )");
        tx.commit();
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
Store::~Store() { sqlite3_close(db_); }

void Store::event(const std::string& task, const std::string& kind, const Json& payload) {
    Statement query(db_, "INSERT INTO events(task_id,kind,payload) VALUES(?,?,?)");
    query.bind(1, task); query.bind(2, kind); query.bind(3, payload.dump()); query.next();
}
std::string Store::create_task(const std::string& prompt, const std::string& workspace,
                                const std::string& provider) {
    const auto id = new_id();
    Transaction tx(db_);
    Statement query(db_, "INSERT INTO tasks(id,prompt,workspace,provider,status) VALUES(?,?,?,?,'running')");
    query.bind(1,id); query.bind(2,prompt); query.bind(3,workspace); query.bind(4,provider); query.next();
    event(id, "task.started", {{"provider",provider},{"workspace",workspace}});
    tx.commit();
    return id;
}
void Store::finish_task(const std::string& id, const std::string& status, const std::string& summary,
                        const Json* checkpoint_state) {
    const std::array allowed{"completed", "needs_attention", "failed", "cancelled", "exhausted", "waiting_approval"};
    if (std::ranges::find(allowed,status) == allowed.end()) throw std::runtime_error("Invalid terminal task status");
    if (status == "waiting_approval" && !checkpoint_state)
        throw std::runtime_error("Waiting for build approval requires a checkpoint");
    Transaction tx(db_);
    {
        Statement unresolved(db_, "SELECT count(*) FROM actions WHERE task_id=? AND status IN ('prepared','dispatched')");
        unresolved.bind(1,id); unresolved.next();
        if (unresolved.number(0) != 0) throw std::runtime_error("In-flight action needs explicit recovery before task can finish");
    }
    if (status == "waiting_approval") {
        if (!checkpoint_state->value("resumable",false))
            throw std::runtime_error("Waiting for build approval requires a resumable checkpoint");
        Statement uncertain(db_, "SELECT count(*) FROM actions WHERE task_id=? AND (status='unknown' OR effect IN ('partial','unknown'))");
        uncertain.bind(1,id); uncertain.next();
        if (uncertain.number(0) != 0)
            throw std::runtime_error("Uncertain actions prevent waiting for build approval");
        (void)pending_proposal(db_,id,*checkpoint_state);
    }
    if (checkpoint_state) save_checkpoint_in_transaction(db_,id,*checkpoint_state,nullptr);
    Statement query(db_, "UPDATE tasks SET status=?,summary=?,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=? AND status='running'");
    query.bind(1,status); query.bind(2,summary); query.bind(3,id); query.next(); require_changed(db_);
    event(id,status == "waiting_approval" ? "task.waiting_approval" : "task.finished",{{"status",status},{"summary",summary}});
    Statement associated(db_,"SELECT id FROM scheduler_jobs WHERE task_id=? AND status='running'");
    associated.bind(1,id);
    if (associated.next()) {
        const auto job=read_schedule(db_,associated.text(0));
        const auto now=current_ms();
        if (now>=job.at("deadline_at_ms").get<std::int64_t>()) scheduler_transition(db_,job,"expired","Scheduled job deadline reached",now);
        else if (status=="completed") scheduler_transition(db_,job,"completed","",now);
        else if (status=="cancelled") scheduler_transition(db_,job,"cancelled","Scheduled task cancelled",now);
        else if (!exhausted_schedule(db_,job,now)) {
            const auto saved=read_checkpoint(db_,id);
            const auto& state=saved.at("state");
            if (status=="waiting_approval") scheduler_transition(db_,job,"waiting_approval","Waiting for an explicit proposal decision",now);
            else if (status=="failed" && state.at("phase")=="model_pending" &&
                state.at("resumable")==true && remaining_budget(state,job) && !uncertain_actions(db_,id)) {
                const auto due=retry_at(job,now);
                scheduler_transition(db_,job,due>=job.at("deadline_at_ms").get<std::int64_t>() ? "expired" : "queued",
                    "Model request failed; retry within the original model budget",now,due);
            } else scheduler_transition(db_,job,status=="exhausted" ||
                (state.at("phase")!="tools" && !remaining_budget(state,job)) ? "exhausted" : "blocked",
                "Task outcome requires explicit review",now);
        }
    } else advance_cron_after_task(db_,id,current_ms());
    tx.commit();
}
std::string Store::prepare_action(const std::string& task, const ToolCall& call) {
    const auto id = new_id();
    Transaction tx(db_);
    Statement query(db_, R"(INSERT INTO actions(id,task_id,call_id,tool,arguments,status)
        SELECT ?,?,?,?,?, 'prepared' WHERE EXISTS(SELECT 1 FROM tasks WHERE id=? AND status='running'))");
    query.bind(1,id); query.bind(2,task); query.bind(3,call.id); query.bind(4,call.name);
    query.bind(5,call.arguments.dump()); query.bind(6,task); query.next(); require_changed(db_);
    event(task,"action.prepared",{{"action_id",id},{"call_id",call.id},{"tool",call.name},{"arguments",call.arguments}});
    tx.commit(); return id;
}
void Store::dispatch_action(const std::string& action) {
    Transaction tx(db_);
    Statement update(db_, "UPDATE actions SET status='dispatched',effect='unknown' WHERE id=? AND status='prepared'");
    update.bind(1,action); update.next(); require_changed(db_);
    Statement query(db_, "SELECT task_id FROM actions WHERE id=?"); query.bind(1,action); query.next();
    event(query.text(0),"action.dispatched",{{"action_id",action}}); tx.commit();
}
void Store::finish_action(const std::string& action, const ToolResult& result, const Json* checkpoint_state) {
    const std::array effects{"none","applied","partial","unknown"};
    if (std::ranges::find(effects,result.effect) == effects.end()) throw std::runtime_error("Invalid action effect");
    Transaction tx(db_);
    Statement update(db_, "UPDATE actions SET status=?,effect=?,result=? WHERE id=? AND status='dispatched'");
    const std::string status = result.effect == "unknown" ? "unknown" : (result.ok ? "succeeded" : "failed");
    Json body{{"ok",result.ok},{"output",result.output},{"error",result.error},{"effect",result.effect}};
    update.bind(1,status); update.bind(2,result.effect); update.bind(3,body.dump()); update.bind(4,action);
    update.next(); require_changed(db_);
    Statement query(db_, "SELECT task_id FROM actions WHERE id=?"); query.bind(1,action); query.next();
    const auto task = query.text(0);
    if (checkpoint_state) save_checkpoint_in_transaction(db_,task,*checkpoint_state,nullptr);
    event(task,"action.finished",{{"action_id",action},{"status",status},{"result",body}}); tx.commit();
}
void Store::deny_action(const std::string& task, const ToolCall& call, const std::string& reason,
                        const Json* checkpoint_state) {
    const auto id = new_id();
    Transaction tx(db_);
    Statement query(db_, R"(INSERT INTO actions(id,task_id,call_id,tool,arguments,status,result)
        SELECT ?,?,?,?,?,'denied',? WHERE EXISTS(SELECT 1 FROM tasks WHERE id=? AND status='running'))");
    query.bind(1,id); query.bind(2,task); query.bind(3,call.id); query.bind(4,call.name);
    query.bind(5,call.arguments.dump()); query.bind(6,Json{{"error",reason}}.dump());
    query.bind(7,task); query.next(); require_changed(db_);
    if (checkpoint_state) save_checkpoint_in_transaction(db_,task,*checkpoint_state,nullptr);
    event(task,"action.denied",{{"action_id",id},{"tool",call.name},{"reason",reason}}); tx.commit();
}
void Store::save_checkpoint(const std::string& task, const Json& state, const Json& binding) {
    Transaction tx(db_);
    save_checkpoint_in_transaction(db_,task,state,&binding);
    tx.commit();
}
Json Store::checkpoint(const std::string& task) const { return read_checkpoint(db_,task); }

void Store::resume_task(const std::string& task, const Json& expected_binding,
                        const std::string& scheduler_job, const std::string& claim_token) {
    Transaction tx(db_);
    Statement current(db_, "SELECT status FROM tasks WHERE id=?");
    current.bind(1,task);
    if (!current.next()) throw std::runtime_error("Task not found");
    const std::array allowed{"queued", "interrupted", "cancelled", "failed", "exhausted", "waiting_approval"};
    const auto previous_status = current.text(0);
    if (std::ranges::find(allowed,previous_status) == allowed.end())
        throw std::runtime_error("Task status cannot resume; recover running tasks first");
    const auto saved = read_checkpoint(db_,task);
    validate_checkpoint(saved.at("state"),expected_binding);
    if (saved.at("binding") != expected_binding)
        throw std::runtime_error("Resume configuration does not match the saved task binding");
    const auto& state = saved.at("state");
    if (!state.at("resumable").get<bool>() || state.at("phase") == "finished")
        throw std::runtime_error("Task checkpoint does not permit resume");
    if (previous_status == "waiting_approval") {
        const auto continuation = proposal_continuation(task,pending_proposal(db_,task,state));
        if (continuation.at("status") != "ready")
            throw std::runtime_error("Build proposal is pending or blocked; task cannot resume");
    }
    Statement uncertain(db_, R"(SELECT EXISTS(SELECT 1 FROM actions WHERE task_id=? AND
        (status IN ('prepared','dispatched','unknown') OR effect IN ('partial','unknown'))))");
    uncertain.bind(1,task); uncertain.next();
    if (uncertain.number(0) != 0)
        throw std::runtime_error("Task has unresolved or uncertain actions and cannot resume");
    if (scheduler_job.empty()) {
        if (!claim_token.empty()) throw std::runtime_error("A scheduler claim requires its job");
        Statement existing(db_,"SELECT id FROM scheduler_jobs WHERE task_id=? AND status IN ('queued','claimed','waiting_approval','running')");
        existing.bind(1,task);
        if (existing.next()) {
            const auto job=read_schedule(db_,existing.text(0));
            if (job.at("status")=="running") throw std::runtime_error("Scheduled task must be recovered before manual resume");
            scheduler_transition(db_,job,"cancelled","Superseded by explicit manual resume",current_ms());
        }
    } else {
        const auto job=read_schedule(db_,scheduler_job);
        const auto control=scheduler();
        if (job.at("task_id")!=task || job.at("status")!="claimed" || claim_token.empty() ||
            job.at("claim_token")!=claim_token || job.at("cancel_requested")==true ||
            job.at("owner")!=control.at("owner") || job.at("generation")!=control.at("generation") ||
            control.at("state")!="running" || current_ms()>=job.at("deadline_at_ms").get<std::int64_t>() ||
            job.at("binding")!=expected_binding || job.at("expected_revision")!=revision(db_,task) ||
            state.value("max_turns",Json())!=job.at("budget_turns") ||
            (state.at("phase")!="tools" && !remaining_budget(state,job)))
            throw std::runtime_error("Scheduled resume claim is stale, cancelled, expired, or outside its authorization");
        Statement start(db_,"UPDATE scheduler_jobs SET status='running',updated_at_ms=? WHERE id=? AND status='claimed' AND claim_token=?");
        start.bind(1,current_ms()); start.bind(2,scheduler_job); start.bind(3,claim_token); start.next(); require_changed(db_);
        event(task,"scheduler.running",{{"job_id",scheduler_job},{"claim_token",claim_token},{"attempts",job.at("attempts")}});
    }
    Statement update(db_, "UPDATE tasks SET status='running',summary='',updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=? AND status=?");
    update.bind(1,task); update.bind(2,previous_status); update.next(); require_changed(db_);
    event(task,"task.resumed",{{"previous_status",previous_status},{"checkpoint_version",2}});
    tx.commit();
}
std::int64_t Store::checkpoint_revision(const std::string& task) const { return revision(db_,task); }

Json Store::schedule_resume(const std::string& task, const Json& resume_argv, const Json& expected_binding,
                           std::int64_t expected_revision, std::int64_t due_ms, std::int64_t deadline_ms,
                           int max_attempts, std::int64_t retry_delay_ms) {
    scheduler_storage::identifier(task);
    const auto now=current_ms();
    if (!due_ms) due_ms=now;
    if (!deadline_ms) deadline_ms=now+24LL*60*60*1000;
    if (due_ms<0 || deadline_ms<=now || deadline_ms<=due_ms || deadline_ms>now+7LL*24*60*60*1000 ||
        max_attempts<1 || max_attempts>100 || retry_delay_ms<100 || retry_delay_ms>60000)
        throw std::invalid_argument("Invalid schedule deadline, attempt limit, or retry delay");
    if (!resume_argv.is_array() || resume_argv.size()<3 || resume_argv.size()>256 ||
        resume_argv.dump().size()>65536 || resume_argv[1]!="resume" || resume_argv[2]!=task)
        throw std::invalid_argument("Scheduled arguments must identify the exact resume task");
    for (const auto& arg : resume_argv) {
        if (!arg.is_string()) throw std::invalid_argument("Scheduled arguments must be strings");
        const auto& value=arg.get_ref<const std::string&>();
        if (value.find('\0')!=std::string::npos || value.starts_with("--api-key") ||
            value.starts_with("--authorization") || value.starts_with("--header") || value.starts_with("--token"))
            throw std::invalid_argument("Scheduled arguments must not contain inline credentials");
    }
    if (expected_binding.dump().size()>262144) throw std::invalid_argument("Scheduled binding exceeds 256 KiB");
    Transaction tx(db_);
    Statement active(db_,"SELECT count(*) FROM scheduler_jobs WHERE status IN ('queued','waiting_approval','claimed','running')");
    active.next();
    if (active.number(0)>=100) throw std::runtime_error("At most 100 active scheduled jobs are supported");
    Statement task_row(db_,"SELECT status FROM tasks WHERE id=?"); task_row.bind(1,task);
    if (!task_row.next()) throw std::runtime_error("Task not found");
    const auto status=task_row.text(0);
    if (status!="queued" && status!="interrupted" && status!="cancelled" && status!="failed" && status!="exhausted" && status!="waiting_approval")
        throw std::runtime_error("Only paused tasks with resumable checkpoints may be scheduled");
    const auto saved=read_checkpoint(db_,task);
    const auto& state=saved.at("state");
    if (saved.at("binding")!=expected_binding || expected_revision!=revision(db_,task) ||
        state.at("resumable")!=true || state.at("phase")=="finished" || uncertain_actions(db_,task) ||
        !state.contains("max_turns") || !state.at("max_turns").is_number_integer() ||
        state.at("max_turns")<1 || state.at("max_turns")>100 ||
        !state.contains("turns") || !state.at("turns").is_number_integer() || state.at("turns")<0 ||
        state.at("turns")>state.at("max_turns") ||
        (state.at("phase")!="tools" && state.at("turns")>=state.at("max_turns")))
        throw std::runtime_error("Scheduled resume requires the exact safe checkpoint and remaining original budget");
    if (status=="waiting_approval") (void)pending_proposal(db_,task,state);
    const auto id=new_id();
    Statement insert(db_,R"(INSERT INTO scheduler_jobs(id,task_id,status,resume_argv,binding,budget_turns,
        expected_revision,due_at_ms,deadline_at_ms,max_attempts,retry_delay_ms,created_at_ms,updated_at_ms)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?))");
    insert.bind(1,id); insert.bind(2,task); insert.bind(3,status=="waiting_approval" ? std::string("waiting_approval") : std::string("queued"));
    insert.bind(4,resume_argv.dump()); insert.bind(5,expected_binding.dump());
    insert.bind(6,state.at("max_turns").get<std::int64_t>()); insert.bind(7,expected_revision);
    insert.bind(8,due_ms); insert.bind(9,deadline_ms); insert.bind(10,max_attempts); insert.bind(11,retry_delay_ms);
    insert.bind(12,now); insert.bind(13,now); insert.next();
    event(task,"scheduler.enqueued",{{"job_id",id},{"expected_revision",expected_revision},
        {"budget_turns",state.at("max_turns")},{"due_at_ms",due_ms},{"deadline_at_ms",deadline_ms}});
    const auto result=read_schedule(db_,id); tx.commit(); return result;
}

Json Store::schedule(const std::string& id) const { return read_schedule(db_,id); }
Json Store::schedules(std::int64_t before, std::size_t limit) const {
    if (before<0 || !limit || limit>100) throw std::invalid_argument("Invalid schedule page cursor or limit");
    const auto query_text=std::string("SELECT ")+scheduler_storage::columns+
        " FROM scheduler_jobs WHERE (?=0 OR rowid<?) ORDER BY rowid DESC LIMIT ?";
    Statement query(db_,query_text.c_str()); query.bind(1,before); query.bind(2,before);
    query.bind(3,static_cast<std::int64_t>(limit+1));
    Json page{{"items",Json::array()},{"next_before",0},{"has_more",false}};
    std::size_t size=128;
    while (query.next()) {
        auto row=scheduler_storage::job(query);
        const auto bytes=row.dump().size()+1;
        if (page["items"].size()==limit || size+bytes>2*1024*1024) { page["has_more"]=true; break; }
        size+=bytes; page["items"].push_back(std::move(row)); page["next_before"]=query.number(19);
    }
    return page;
}
Json Store::pending_schedules() const {
    const auto query_text=std::string("SELECT ")+scheduler_storage::columns+
        " FROM scheduler_jobs WHERE status IN ('queued','waiting_approval','claimed','running') ORDER BY due_at_ms,rowid LIMIT 100";
    Statement query(db_,query_text.c_str()); Json result=Json::array();
    while (query.next()) result.push_back(scheduler_storage::job(query));
    return result;
}

Json Store::claim_schedule(const std::string& id, const std::string& owner,
                          std::int64_t generation, std::int64_t now_ms) {
    Transaction tx(db_);
    const auto control=scheduler();
    if (control.at("state")!="running" || control.at("owner")!=owner || control.at("generation")!=generation)
        throw std::runtime_error("Scheduler ownership is stale or stopped");
    const auto job=read_schedule(db_,id);
    if (job.at("status")!="queued" && job.at("status")!="waiting_approval") return nullptr;
    if (exhausted_schedule(db_,job,now_ms)) { tx.commit(); return nullptr; }
    if (now_ms<job.at("due_at_ms").get<std::int64_t>()) return nullptr;
    const auto task=job.at("task_id").get<std::string>();
    Statement task_row(db_,"SELECT status FROM tasks WHERE id=?"); task_row.bind(1,task); task_row.next();
    const auto saved=read_checkpoint(db_,task);
    const auto& state=saved.at("state");
    if (job.at("expected_revision")!=revision(db_,task) || job.at("binding")!=saved.at("binding") ||
        state.at("resumable")!=true || state.at("phase")=="finished" || uncertain_actions(db_,task) ||
        state.value("max_turns",Json())!=job.at("budget_turns") || task_row.text(0)=="running" ||
        task_row.text(0)=="completed" || task_row.text(0)=="needs_attention") {
        scheduler_transition(db_,job,"blocked","Checkpoint, task state, or authorization changed",now_ms);
        tx.commit(); return nullptr;
    }
    if (state.at("phase")!="tools" && !remaining_budget(state,job)) {
        scheduler_transition(db_,job,"exhausted","Original model turn budget reached",now_ms); tx.commit(); return nullptr;
    }
    if (task_row.text(0)=="waiting_approval") {
        const auto continuation=proposal_continuation(task,pending_proposal(db_,task,state));
        if (continuation.at("status")=="pending") return nullptr;
        if (continuation.at("status")!="ready") {
            scheduler_transition(db_,job,"blocked","Build proposal outcome requires explicit review",now_ms); tx.commit(); return nullptr;
        }
    }
    const auto token=new_id();
    Statement claim(db_,R"(UPDATE scheduler_jobs SET status='claimed',attempts=attempts+1,owner=?,generation=?,
        claim_token=?,updated_at_ms=?,last_error='' WHERE id=? AND status=?)");
    claim.bind(1,owner); claim.bind(2,generation); claim.bind(3,token); claim.bind(4,now_ms); claim.bind(5,id);
    claim.bind(6,job.at("status").get<std::string>()); claim.next(); require_changed(db_);
    event(task,"scheduler.claimed",{{"job_id",id},{"owner",owner},{"generation",generation},
        {"claim_token",token},{"attempts",job.at("attempts").get<int>()+1}});
    const auto result=read_schedule(db_,id); tx.commit(); return result;
}

void Store::finish_scheduler_claim(const std::string& id, const std::string& claim_token,
                                  const std::string& reason, std::int64_t now_ms) {
    Transaction tx(db_); const auto job=read_schedule(db_,id);
    if (job.at("status")!="claimed" || claim_token.empty() || job.at("claim_token")!=claim_token) return;
    scheduler_transition(db_,job,job.at("cancel_requested")==true ? "cancelled" : "blocked",reason,now_ms);
    tx.commit();
}
Json Store::cancel_schedule(const std::string& id, std::int64_t now_ms, bool active_only) {
    Transaction tx(db_); const auto job=read_schedule(db_,id);
    if (active_only && job.at("status")!="claimed" && job.at("status")!="running") {
        tx.commit(); return job;
    }
    if (job.at("status")=="running") {
        if (job.at("cancel_requested")!=true) {
            Statement cancel(db_,"UPDATE scheduler_jobs SET cancel_requested=1,updated_at_ms=? WHERE id=?");
            cancel.bind(1,now_ms); cancel.bind(2,id); cancel.next();
            event(job.at("task_id").get<std::string>(),"scheduler.cancel_requested",{{"job_id",id}});
        }
    } else if (job.at("status")=="queued" || job.at("status")=="claimed" || job.at("status")=="waiting_approval")
        scheduler_transition(db_,job,"cancelled","Scheduled job cancelled",now_ms);
    const auto result=read_schedule(db_,id); tx.commit(); return result;
}

std::int64_t Store::start_scheduler(const std::string& owner, std::int64_t pid, std::int64_t now_ms,
                                  std::optional<SchedulerStartFence> expected) {
    scheduler_storage::identifier(owner);
    if (pid<=0 || now_ms<=0) throw std::invalid_argument("Scheduler process and start time must be positive");
    Transaction tx(db_);
    const auto previous=scheduler();
    if (expected && (expected->stop_epoch<0 || expected->generation<0 ||
        previous.at("stop_epoch")!=expected->stop_epoch || previous.at("generation")!=expected->generation))
        throw std::runtime_error("Scheduler start fence changed; refusing stale child launch");
    if (previous.at("generation")==INT64_MAX) throw std::runtime_error("Scheduler generation exhausted");
    Statement update(db_,R"(UPDATE scheduler_control SET generation=generation+1,owner=?,pid=?,
        started_at_ms=?,heartbeat_at_ms=?,state='running' WHERE singleton=1)");
    update.bind(1,owner); update.bind(2,pid); update.bind(3,now_ms); update.bind(4,now_ms); update.next(); require_changed(db_);
    const auto generation=scheduler().at("generation").get<std::int64_t>(); tx.commit(); return generation;
}
bool Store::heartbeat_scheduler(const std::string& owner, std::int64_t now_ms) {
    Statement update(db_,"UPDATE scheduler_control SET heartbeat_at_ms=? WHERE singleton=1 AND owner=? AND state='running'");
    update.bind(1,now_ms); update.bind(2,owner); update.next(); return sqlite3_changes(db_)==1;
}
Json Store::stop_scheduler(std::optional<SchedulerStartFence> expected) {
    Transaction tx(db_);
    const auto previous=scheduler();
    if (expected && (previous.at("stop_epoch")!=expected->stop_epoch || previous.at("generation")!=expected->generation)) {
        tx.commit(); return previous;
    }
    if (previous.at("stop_epoch")==INT64_MAX) throw std::runtime_error("Scheduler stop epoch exhausted");
    sql(db_,"UPDATE scheduler_control SET stop_epoch=stop_epoch+1,state='stopped' WHERE singleton=1");
    const auto control=scheduler();
    Statement query(db_,"SELECT id,task_id FROM scheduler_jobs WHERE status IN ('queued','claimed','running','waiting_approval')");
    while (query.next()) event(query.text(1),"scheduler.stop_requested",{{"job_id",query.text(0)},{"stop_epoch",control.at("stop_epoch")}});
    tx.commit(); return control;
}
void Store::finish_scheduler(const std::string& owner) {
    Statement update(db_,"UPDATE scheduler_control SET state='stopped' WHERE singleton=1 AND owner=?");
    update.bind(1,owner); update.next();
}
Json Store::scheduler() const {
    Statement query(db_,"SELECT state,owner,pid,generation,stop_epoch,started_at_ms,heartbeat_at_ms FROM scheduler_control WHERE singleton=1");
    if (!query.next()) throw std::runtime_error("Scheduler control is missing");
    return scheduler_storage::control(query);
}

namespace {
Json read_supervisor(sqlite3* db, const std::string& id) {
    const auto text=std::string("SELECT ")+supervisor_storage::columns+" FROM supervisor_runs WHERE id=?";
    Statement query(db,text.c_str()); query.bind(1,id);
    if (!query.next()) throw std::runtime_error("Supervisor lifecycle is missing");
    return supervisor_storage::run(query);
}
void supervisor_event(sqlite3* db, const Json& run, const std::string& kind, std::int64_t now_ms) {
    Statement event(db,"INSERT INTO supervisor_events(run_id,kind,payload,created_at_ms) VALUES(?,?,?,?)");
    event.bind(1,run.at("id").get<std::string>()); event.bind(2,kind);
    event.bind(3,run.dump()); event.bind(4,now_ms); event.next();
}
}

Json Store::supervisor() const {
    const auto text=std::string("SELECT ")+supervisor_storage::columns+" FROM supervisor_runs ORDER BY rowid DESC LIMIT 1";
    Statement query(db_,text.c_str());
    return query.next() ? supervisor_storage::run(query) : Json(nullptr);
}
void Store::start_supervisor(const SupervisorRunOptions& options, SchedulerStartFence expected, std::int64_t now_ms) {
    supervisor_storage::options(options);
    if (now_ms<=0 || expected.stop_epoch<0 || expected.generation<0)
        throw std::invalid_argument("Invalid supervisor start time or scheduler fence");
    Transaction tx(db_);
    const auto control=scheduler();
    if (control.at("stop_epoch")!=expected.stop_epoch || control.at("generation")!=expected.generation)
        throw std::runtime_error("Supervisor start fence changed; refusing stale registration");
    const auto previous=supervisor();
    if (!previous.is_null() && previous.at("phase")!="stopped") {
        // Admission ownership proves replacement is authorized, not that an old
        // child exit was observed. Preserve its nullable exit code unchanged.
        Statement replace(db_,"UPDATE supervisor_runs SET phase='stopped',worker_pid=0,next_restart_at_ms=0,exit_reason='superseded',updated_at_ms=?,finished_at_ms=? WHERE id=?");
        const auto replaced_at=std::max(now_ms,previous.at("updated_at_ms").get<std::int64_t>());
        replace.bind(1,replaced_at); replace.bind(2,replaced_at); replace.bind(3,previous.at("id").get<std::string>());
        replace.next(); require_changed(db_);
        supervisor_event(db_,read_supervisor(db_,previous.at("id").get<std::string>()),"supervisor.stopped",replaced_at);
    }
    Statement insert(db_,R"(INSERT INTO supervisor_runs(id,owner,workspace,process_identity,pid,poll_ms,max_restarts,
        restart_delay_ms,phase,generation,stop_epoch,started_at_ms,updated_at_ms,heartbeat_at_ms)
        VALUES(?,?,?,?,?,?,?,?,'starting',?,?,?,?,?))");
    insert.bind(1,options.id); insert.bind(2,options.owner); insert.bind(3,options.workspace);
    insert.bind(4,options.process_identity); insert.bind(5,options.pid); insert.bind(6,options.poll_ms);
    insert.bind(7,options.max_restarts); insert.bind(8,options.restart_delay_ms);
    insert.bind(9,expected.generation); insert.bind(10,expected.stop_epoch);
    insert.bind(11,now_ms); insert.bind(12,now_ms); insert.bind(13,now_ms); insert.next();
    supervisor_event(db_,read_supervisor(db_,options.id),"supervisor.starting",now_ms);
    tx.commit();
}
bool Store::update_supervisor(const std::string& id, const std::string& owner,
                              const SupervisorRunState& state, std::int64_t now_ms,
                              const std::string& event_kind) {
    scheduler_storage::identifier(id); scheduler_storage::identifier(owner);
    if (now_ms<=0) throw std::invalid_argument("Supervisor update time must be positive");
    Transaction tx(db_);
    const auto previous=supervisor();
    if (previous.is_null() || previous.at("id")!=id || previous.at("owner")!=owner || previous.at("phase")=="stopped") {
        tx.commit(); return false;
    }
    // Wall-clock corrections must not turn a diagnostic write into lost
    // ownership. Ordering within this record stays monotonic; events use seq.
    now_ms=std::max(now_ms,previous.at("updated_at_ms").get<std::int64_t>());
    supervisor_storage::transition(previous,state,event_kind);
    Statement update(db_,R"(UPDATE supervisor_runs SET phase=?,restarts=?,worker_pid=?,generation=?,next_restart_at_ms=?,
        last_exit_code=?,exit_reason=?,updated_at_ms=?,heartbeat_at_ms=?,finished_at_ms=? WHERE id=? AND owner=?)");
    update.bind(1,state.phase); update.bind(2,state.restarts); update.bind(3,state.worker_pid);
    update.bind(4,state.generation); update.bind(5,state.next_restart_at_ms);
    if (state.last_exit_code) update.bind(6,*state.last_exit_code); else update.bind_null(6);
    update.bind(7,state.exit_reason); update.bind(8,now_ms); update.bind(9,now_ms);
    update.bind(10,state.phase=="stopped" ? now_ms : 0); update.bind(11,id); update.bind(12,owner);
    update.next(); require_changed(db_);
    supervisor_event(db_,read_supervisor(db_,id),event_kind,now_ms);
    tx.commit(); return true;
}
bool Store::heartbeat_supervisor(const std::string& id, const std::string& owner, std::int64_t now_ms) {
    scheduler_storage::identifier(id); scheduler_storage::identifier(owner);
    if (now_ms<=0) throw std::invalid_argument("Supervisor heartbeat time must be positive");
    Statement update(db_,R"(UPDATE supervisor_runs SET heartbeat_at_ms=max(updated_at_ms,?),updated_at_ms=max(updated_at_ms,?)
        WHERE id=? AND owner=? AND phase!='stopped'
        AND rowid=(SELECT max(rowid) FROM supervisor_runs))");
    update.bind(1,now_ms); update.bind(2,now_ms); update.bind(3,id); update.bind(4,owner);
    update.next(); return sqlite3_changes(db_)==1;
}

Json Store::recover_schedules(const std::string& owner, std::int64_t generation, std::int64_t now_ms) {
    Transaction tx(db_);
    const auto control=scheduler();
    if (control.at("owner")!=owner || control.at("generation")!=generation || control.at("state")!="running")
        throw std::runtime_error("Scheduler recovery requires current scheduler ownership");
    Statement query(db_,"SELECT id FROM scheduler_jobs WHERE status IN ('claimed','running')");
    std::vector<std::string> ids; while (query.next()) ids.push_back(query.text(0));
    Json result=Json::array();
    for (const auto& id : ids) {
        const auto job=read_schedule(db_,id); const auto task=job.at("task_id").get<std::string>();
        if (job.at("status")=="running") {
            Statement running(db_,"SELECT status FROM tasks WHERE id=?"); running.bind(1,task); running.next();
            if (running.text(0)=="running") {
                Statement unknown(db_,"UPDATE actions SET status='unknown',effect='unknown' WHERE task_id=? AND status='dispatched'");
                unknown.bind(1,task); unknown.next(); const auto unknown_count=sqlite3_changes(db_);
                Statement prepared(db_,"UPDATE actions SET status='not_executed' WHERE task_id=? AND status='prepared'");
                prepared.bind(1,task); prepared.next();
                Statement interrupted(db_,"UPDATE tasks SET status='interrupted',summary='Scheduled runner interrupted',updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=? AND status='running'");
                interrupted.bind(1,task); interrupted.next(); require_changed(db_);
                event(task,"scheduler.task_recovered",{{"job_id",id},{"unknown_actions",unknown_count}});
            }
        }
        if (!exhausted_schedule(db_,job,now_ms)) {
            const auto saved=read_checkpoint(db_,task); const auto& state=saved.at("state");
            if (uncertain_actions(db_,task) || state.at("resumable")!=true || state.at("phase")=="finished" ||
                job.at("binding")!=saved.at("binding") || job.at("expected_revision")!=revision(db_,task) ||
                state.value("max_turns",Json())!=job.at("budget_turns"))
                scheduler_transition(db_,job,"blocked","Interrupted job has an uncertain receipt or changed checkpoint",now_ms);
            else if (state.at("phase")!="tools" && !remaining_budget(state,job))
                scheduler_transition(db_,job,"exhausted","Original model turn budget reached",now_ms);
            else scheduler_transition(db_,job,"queued","Recovered interrupted runner with durable receipts",now_ms,now_ms);
        }
        result.push_back(read_schedule(db_,id));
    }
    tx.commit(); return result;
}

bool Store::has_running_tasks() const {
    Statement query(db_, "SELECT EXISTS(SELECT 1 FROM tasks WHERE status='running')");
    query.next(); return query.number(0) != 0;
}

Json Store::create_cron(const std::string& name,const std::string& expression,
    const std::string& timezone,const std::string& prompt,const std::string& provider,
    const Json& resume_argv,const Json& initial_checkpoint,std::int64_t deadline_ms,
    int max_attempts,std::int64_t retry_delay_ms,std::int64_t max_runs,std::int64_t now_ms) {
    bounded_cron_text(name,128,"name"); bounded_cron_text(expression,512,"expression");
    bounded_cron_text(timezone,256,"timezone"); bounded_cron_text(prompt,65536,"prompt");
    bounded_cron_text(provider,256,"provider");
    if (now_ms<=0 || now_ms>INT64_MAX-604800000 || deadline_ms<1 || deadline_ms>604800000 ||
        max_attempts<1 || max_attempts>100 || retry_delay_ms<100 || retry_delay_ms>60000 ||
        max_runs<0 || max_runs>1000000) throw std::invalid_argument("Invalid cron limits or time");
    const auto zone=cron_timezone(timezone);
    validate_cron(expression,zone);
    const auto next=cron_next(expression,zone,now_ms);
    if (!initial_checkpoint.is_object() || initial_checkpoint.value("version",Json())!=2 ||
        !initial_checkpoint.at("version").is_number_integer() ||
        !initial_checkpoint.contains("state") || !initial_checkpoint.contains("binding"))
        throw std::invalid_argument("Cron requires a current initial checkpoint");
    const auto& state=initial_checkpoint.at("state");
    const auto& binding=initial_checkpoint.at("binding");
    validate_checkpoint(state,binding);
    if (state.dump().size()>262144 || binding.dump().size()>262144 ||
        state.at("phase")!="ready" || state.at("resumable")!=true ||
        !state.contains("turns") || !state.at("turns").is_number_integer() || state.at("turns")!=0 ||
        !state.contains("max_turns") || !state.at("max_turns").is_number_integer() ||
        state.at("max_turns")<1 || state.at("max_turns")>100 ||
        state.value("pending_calls",Json())!=Json::array() || state.value("calls_seen",Json())!=Json::array() ||
        !state.contains("next_call") || !state.at("next_call").is_number_integer() || state.at("next_call")!=0 ||
        state.value("tool_failed",Json())!=false || state.value("blocked_reason",Json())!="" ||
        !state.contains("messages") || !state.at("messages").is_array() || state.at("messages").size()!=2 ||
        state.at("messages")[0].value("role",Json())!="system" ||
        state.at("messages")[1].value("role",Json())!="user" ||
        state.at("messages")[1].value("content",Json())!=prompt ||
        binding.value("provider",Json())!=provider || !binding.contains("workspace") ||
        !binding.at("workspace").is_string())
        throw std::invalid_argument("Cron template must be an unused ready task with its exact prompt and binding");
    const auto workspace=binding.at("workspace").get<std::string>();
    bounded_cron_text(workspace,32768,"workspace");
    if (!resume_argv.is_array() || resume_argv.size()<7 || resume_argv.size()>256 ||
        resume_argv.dump().size()>65536 || resume_argv[1]!="resume" || resume_argv[2]!="cron-task")
        throw std::invalid_argument("Cron arguments require the resume template placeholder");
    bool workspace_seen=false,state_seen=false;
    for (std::size_t index=0;index<resume_argv.size();++index) {
        if (!resume_argv[index].is_string()) throw std::invalid_argument("Cron arguments must be strings");
        const auto& value=resume_argv[index].get_ref<const std::string&>();
        if (value.find('\0')!=std::string::npos || value.starts_with("--api-key") ||
            value.starts_with("--authorization") || value.starts_with("--header") || value.starts_with("--token") ||
            value=="--prompt") throw std::invalid_argument("Cron arguments must not contain inline credentials or a replacement prompt");
        if (value=="--workspace") {
            if (workspace_seen || index+1==resume_argv.size() || resume_argv[index+1]!=workspace)
                throw std::invalid_argument("Cron workspace differs from its checkpoint");
            workspace_seen=true;
        }
        if (value=="--state") {
            if (state_seen || index+1==resume_argv.size() || !resume_argv[index+1].is_string() ||
                resume_argv[index+1].get_ref<const std::string&>().empty())
                throw std::invalid_argument("Cron state path must be explicit");
            state_seen=true;
        }
    }
    if (!workspace_seen || !state_seen) throw std::invalid_argument("Cron workspace and state must be explicit");
    Transaction tx(db_);
    Statement count(db_,"SELECT count(*) FROM cron_schedules WHERE status!='cancelled'"); count.next();
    if (count.number(0)>=100) throw std::runtime_error("At most 100 non-cancelled cron schedules are supported");
    const auto id=new_id();
    Statement insert(db_,R"(INSERT INTO cron_schedules(id,name,expression,timezone,status,workspace,prompt,provider,
        resume_argv,initial_state,binding,deadline_ms,max_attempts,retry_delay_ms,max_runs,next_due_at_ms,created_at_ms,updated_at_ms)
        VALUES(?,?,?,?,'active',?,?,?,?,?,?,?,?,?,?,?,?,?))");
    insert.bind(1,id); insert.bind(2,name); insert.bind(3,expression); insert.bind(4,zone);
    insert.bind(5,workspace); insert.bind(6,prompt); insert.bind(7,provider); insert.bind(8,resume_argv.dump());
    insert.bind(9,state.dump()); insert.bind(10,binding.dump()); insert.bind(11,deadline_ms);
    insert.bind(12,max_attempts); insert.bind(13,retry_delay_ms); insert.bind(14,max_runs);
    insert.bind(15,next); insert.bind(16,now_ms); insert.bind(17,now_ms); insert.next();
    const auto result=read_cron(db_,id); tx.commit(); return result;
}

Json Store::cron(const std::string& id) const { return read_cron(db_,id); }
Json Store::crons(std::int64_t before,std::size_t limit) const {
    if (before<0 || !limit || limit>100) throw std::invalid_argument("Invalid cron page cursor or limit");
    const auto text=std::string("SELECT ")+cron_storage::columns+
        " FROM cron_schedules WHERE (?=0 OR rowid<?) ORDER BY rowid DESC LIMIT ?";
    Statement query(db_,text.c_str()); query.bind(1,before); query.bind(2,before);
    query.bind(3,static_cast<std::int64_t>(limit+1));
    Json page{{"items",Json::array()},{"next_before",0},{"has_more",false}};
    std::size_t size=128;
    while (query.next()) {
        auto row=cron_storage::definition(query); const auto bytes=row.dump().size()+1;
        if (page["items"].size()==limit || size+bytes>2*1024*1024) { page["has_more"]=true; break; }
        size+=bytes; page["items"].push_back(std::move(row)); page["next_before"]=query.number(cron_storage::cursor_column);
    }
    return page;
}

Json Store::control_cron(const std::string& id,const std::string& action,std::int64_t now_ms) {
    if ((action!="pause" && action!="resume" && action!="cancel") || now_ms<=0)
        throw std::invalid_argument("Invalid cron control action or time");
    Transaction tx(db_); const auto record=read_cron(db_,id);
    if (action=="cancel" && record.at("status")=="cancelled") return record;
    if (record.at("status")=="cancelled") throw std::runtime_error("Cancelled cron schedules cannot be re-enabled");
    Json last;
    if (!record.at("last_job_id").get_ref<const std::string&>().empty())
        last=read_schedule(db_,record.at("last_job_id").get<std::string>());
    if (action=="cancel") {
        if (!last.is_null() && active_schedule(last)) {
            if (last.at("status")=="running") {
                if (last.at("cancel_requested")!=true) {
                    Statement cancel(db_,"UPDATE scheduler_jobs SET cancel_requested=1,updated_at_ms=? WHERE id=?");
                    cancel.bind(1,now_ms); cancel.bind(2,last.at("id").get<std::string>()); cancel.next(); require_changed(db_);
                    event(last.at("task_id").get<std::string>(),"scheduler.cancel_requested",{{"job_id",last.at("id")},{"cron_id",id}});
                }
            } else scheduler_transition(db_,last,"cancelled","Cron schedule cancelled",now_ms);
        }
    } else {
        if (record.at("max_runs")!=0 && record.at("run_count")>=record.at("max_runs"))
            throw std::runtime_error("Cron occurrence budget is exhausted");
        if (action=="resume" && !last.is_null() &&
            (last.at("status")=="blocked" || (!active_schedule(last) && uncertain_actions(db_,last.at("task_id").get<std::string>()))))
            throw std::runtime_error("Cron outcome requires explicit review; unresolved work cannot be repeated");
        if ((action=="pause" && record.at("status")=="paused") ||
            (action=="resume" && record.at("status")=="active")) return record;
    }
    const auto status=action=="cancel" ? "cancelled" : action=="pause" ? "paused" : "active";
    const auto next=action=="resume" ? cron_next(record.at("expression").get<std::string>(),
        record.at("timezone").get<std::string>(),std::max(now_ms,record.at("last_due_at_ms").get<std::int64_t>())) : record.at("next_due_at_ms").get<std::int64_t>();
    Statement update(db_,"UPDATE cron_schedules SET status=?,next_due_at_ms=?,updated_at_ms=?,last_error='' WHERE id=?");
    update.bind(1,std::string(status)); update.bind(2,next); update.bind(3,now_ms); update.bind(4,id); update.next(); require_changed(db_);
    const auto result=read_cron(db_,id); tx.commit(); return result;
}

Json Store::materialize_crons(const std::string& workspace,const std::string& owner,
                              std::int64_t generation,std::int64_t now_ms) {
    if (now_ms<=0 || now_ms>INT64_MAX-604800000) throw std::invalid_argument("Invalid cron materialization time");
    Transaction tx(db_); const auto control=scheduler();
    if (control.at("state")!="running" || control.at("owner")!=owner || control.at("generation")!=generation)
        throw std::runtime_error("Cron materialization requires current scheduler ownership");
    Statement due(db_,"SELECT id FROM cron_schedules WHERE workspace=? AND status='active' ORDER BY next_due_at_ms,rowid LIMIT 100");
    due.bind(1,workspace); std::vector<std::string> ids;
    while (due.next()) ids.push_back(due.text(0));
    Statement active(db_,"SELECT count(*) FROM scheduler_jobs WHERE status IN ('queued','claimed','running','waiting_approval')");
    active.next(); auto capacity=100-active.number(0);
    Json created=Json::array();
    for (const auto& id : ids) {
        const auto record=read_cron(db_,id); Json previous;
        bool overlapping=false;
        if (!record.at("last_job_id").get_ref<const std::string&>().empty()) {
            previous=read_schedule(db_,record.at("last_job_id").get<std::string>());
            overlapping=active_schedule(previous);
            Statement prior_task(db_,"SELECT status FROM tasks WHERE id=?");
            prior_task.bind(1,previous.at("task_id").get<std::string>()); prior_task.next();
            overlapping=overlapping || prior_task.text(0)=="running" || prior_task.text(0)=="waiting_approval";
            if (!overlapping && (previous.at("status")=="blocked" || uncertain_actions(db_,previous.at("task_id").get<std::string>()))) {
                Statement pause(db_,"UPDATE cron_schedules SET status='paused',last_error=?,updated_at_ms=? WHERE id=? AND status='active'");
                pause.bind(1,std::string("Previous occurrence has a blocked or uncertain outcome; explicit review is required"));
                pause.bind(2,now_ms); pause.bind(3,id); pause.next(); require_changed(db_); continue;
            }
        }
        if (record.at("max_runs")!=0 && record.at("run_count")>=record.at("max_runs")) {
            Statement exhausted(db_,"UPDATE cron_schedules SET status='exhausted',updated_at_ms=? WHERE id=? AND status='active'");
            exhausted.bind(1,now_ms); exhausted.bind(2,id); exhausted.next(); require_changed(db_); continue;
        }
        if (record.at("next_due_at_ms").get<std::int64_t>()>now_ms) continue;
        if (!overlapping && capacity<=0) continue;
        const auto expression=record.at("expression").get<std::string>();
        const auto zone=record.at("timezone").get<std::string>();
        const auto latest=cron_previous(expression,zone,now_ms);
        const auto next=cron_next(expression,zone,std::max(now_ms,record.at("last_due_at_ms").get<std::int64_t>()));
        if (overlapping || latest<record.at("next_due_at_ms").get<std::int64_t>() ||
            latest<=record.at("last_due_at_ms").get<std::int64_t>()) {
            Statement skip(db_,"UPDATE cron_schedules SET next_due_at_ms=?,updated_at_ms=? WHERE id=? AND status='active'");
            skip.bind(1,next); skip.bind(2,now_ms); skip.bind(3,id); skip.next(); require_changed(db_); continue;
        }
        Statement template_row(db_,"SELECT initial_state FROM cron_schedules WHERE id=?"); template_row.bind(1,id); template_row.next();
        if (template_row.bytes(0)>262144) throw std::runtime_error("Cron initial checkpoint exceeds read limit");
        const auto state=protocol::strict_json(template_row.text(0)); const auto& binding=record.at("binding");
        validate_checkpoint(state,binding);
        if (state.at("phase")!="ready" || state.at("resumable")!=true || state.at("turns")!=0 ||
            state.at("max_turns")<1 || state.at("max_turns")>100 || binding.at("workspace")!=workspace ||
            binding.at("provider")!=record.at("provider")) throw std::runtime_error("Cron initial checkpoint identity changed");
        const auto task=new_id(),job_id=new_id(); auto argv=record.at("resume_argv"); argv[2]=task;
        Statement task_insert(db_,"INSERT INTO tasks(id,prompt,workspace,provider,status) VALUES(?,?,?,?,'queued')");
        task_insert.bind(1,task); task_insert.bind(2,record.at("prompt").get<std::string>());
        task_insert.bind(3,workspace); task_insert.bind(4,record.at("provider").get<std::string>()); task_insert.next();
        Statement checkpoint(db_,"INSERT INTO task_checkpoints(task_id,version,state,binding,revision) VALUES(?,2,?,?,1)");
        checkpoint.bind(1,task); checkpoint.bind(2,state.dump()); checkpoint.bind(3,binding.dump()); checkpoint.next();
        Statement job(db_,R"(INSERT INTO scheduler_jobs(id,task_id,status,resume_argv,binding,budget_turns,
            expected_revision,due_at_ms,deadline_at_ms,max_attempts,retry_delay_ms,created_at_ms,updated_at_ms)
            VALUES(?,?,'queued',?,?,?,1,?,?,?,?,?,?))");
        job.bind(1,job_id); job.bind(2,task); job.bind(3,argv.dump()); job.bind(4,binding.dump());
        job.bind(5,state.at("max_turns").get<std::int64_t>()); job.bind(6,now_ms);
        job.bind(7,now_ms+record.at("deadline_ms").get<std::int64_t>());
        job.bind(8,record.at("max_attempts").get<std::int64_t>()); job.bind(9,record.at("retry_delay_ms").get<std::int64_t>());
        job.bind(10,now_ms); job.bind(11,now_ms); job.next();
        Statement occurrence(db_,"INSERT INTO cron_occurrences(cron_id,scheduled_at_ms,task_id,job_id) VALUES(?,?,?,?)");
        occurrence.bind(1,id); occurrence.bind(2,latest); occurrence.bind(3,task); occurrence.bind(4,job_id); occurrence.next();
        const auto runs=record.at("run_count").get<std::int64_t>()+1;
        Statement advance(db_,R"(UPDATE cron_schedules SET run_count=?,last_due_at_ms=?,next_due_at_ms=?,last_job_id=?,
            updated_at_ms=?,last_error='',status=? WHERE id=? AND status='active')");
        advance.bind(1,runs); advance.bind(2,latest); advance.bind(3,next); advance.bind(4,job_id); advance.bind(5,now_ms);
        advance.bind(6,std::string(record.at("max_runs")!=0 && runs>=record.at("max_runs").get<std::int64_t>() ? "exhausted" : "active"));
        advance.bind(7,id); advance.next(); require_changed(db_);
        event(task,"task.scheduled",{{"cron_id",id},{"job_id",job_id},{"scheduled_at_ms",latest},{"occurrence",runs}});
        event(task,"scheduler.enqueued",{{"cron_id",id},{"job_id",job_id},{"expected_revision",1},
            {"budget_turns",state.at("max_turns")},{"due_at_ms",now_ms},{"deadline_at_ms",now_ms+record.at("deadline_ms").get<std::int64_t>()}});
        created.push_back(read_schedule(db_,job_id)); --capacity;
    }
    tx.commit(); return created;
}
Json Store::list_tasks() const {
    Statement query(db_, "SELECT id,prompt,workspace,provider,status,summary,created_at,updated_at FROM tasks ORDER BY rowid DESC LIMIT 100");
    Json rows = Json::array(); while(query.next()) rows.push_back(task_row(query)); return rows;
}
Json Store::task(const std::string& id) const {
    ReadSnapshot snapshot(db_);
    Statement query(db_, "SELECT id,prompt,workspace,provider,status,summary,created_at,updated_at FROM tasks WHERE id=?");
    query.bind(1,id); if (!query.next()) throw std::runtime_error("Task not found");
    auto row = task_row(query); row["actions"] = Json::array();
    Statement actions(db_, "SELECT id,call_id,tool,arguments,status,effect,result FROM actions WHERE task_id=? ORDER BY rowid");
    actions.bind(1,id);
    while(actions.next()) {
        Json action{{"id",actions.text(0)},{"call_id",actions.text(1)},
            {"tool",actions.text(2)},{"arguments",Json::parse(actions.text(3))},{"status",actions.text(4)},
            {"effect",actions.text(5)},{"result",Json::parse(actions.text(6))}};
        if (action.at("tool") == "propose_build" && action.at("status") == "succeeded" && action.at("effect") == "none") {
            action["continuation"] = proposal_continuation(id,action.at("id").get<std::string>());
            action["continuation_consumed"] = proposal_was_consumed(db_,id,action.at("id").get<std::string>());
        }
        row["actions"].push_back(std::move(action));
    }
    row["checkpoint"] = nullptr;
    Statement saved(db_, "SELECT EXISTS(SELECT 1 FROM task_checkpoints WHERE task_id=?)");
    saved.bind(1,id); saved.next();
    if (saved.number(0) != 0) {
        const auto envelope = read_checkpoint(db_,id);
        const auto& state = envelope.at("state");
        Json summary{{"version",envelope.at("version")},{"phase",state.at("phase")},
                     {"resumable",state.at("resumable")}};
        for (const auto* name : {"turns", "max_turns"})
            if (state.contains(name) && state.at(name).is_number_integer()) summary[name] = state.at(name);
        if (state.contains("blocked_reason") && state.at("blocked_reason").is_string()) {
            const auto& reason = state.at("blocked_reason").get_ref<const std::string&>();
            auto length = std::min<std::size_t>(reason.size(),1024);
            while (length < reason.size() && length > 0 &&
                   (static_cast<unsigned char>(reason[length]) & 0xc0) == 0x80) --length;
            summary["blocked_reason"] = reason.substr(0,length);
        }
        row["checkpoint"] = std::move(summary);
    }
    return row;
}
Json Store::events(const std::string& id) const {
    Statement query(db_, "SELECT seq,kind,payload,created_at,schema_version FROM events WHERE task_id=? ORDER BY seq");
    query.bind(1,id); Json rows = Json::array();
    while(query.next()) {
        if (query.text(4) != "2") throw std::runtime_error("Unsupported event schema version");
        rows.push_back({{"seq",query.number(0)},{"kind",query.text(1)},
            {"payload",protocol::strict_json(query.text(2))},{"created_at",query.text(3)},{"schema_version",query.number(4)}});
    }
    return rows;
}
Json Store::build_proposal(const std::string& task, const std::string& action) const {
    for (const auto* id : {&task, &action})
        if (id->empty() || id->size() > 256 || id->find('\0') != std::string::npos)
            throw std::invalid_argument("Proposal identifiers must contain 1..256 bytes without NUL");
    Statement query(db_, R"(SELECT t.workspace,a.tool,a.status,a.effect,
        length(CAST(a.arguments AS BLOB)),length(CAST(a.result AS BLOB)),a.arguments,a.result
        FROM actions a JOIN tasks t ON t.id=a.task_id WHERE a.task_id=? AND a.id=?)");
    query.bind(1,task); query.bind(2,action);
    if (!query.next()) throw std::runtime_error("Build proposal not found");
    if (query.bytes(0) > 32768 || query.bytes(1) > 256 || query.bytes(2) > 256 || query.bytes(3) > 256 ||
        query.number(4) > 256 * 1024 || query.number(5) > 256 * 1024)
        throw std::runtime_error("Build proposal exceeds read size limit");
    if (query.text(1) != "propose_build" || query.text(2) != "succeeded" || query.text(3) != "none")
        throw std::runtime_error("Action is not a successful build proposal");
    auto arguments = protocol::strict_json(query.text(6));
    auto result = protocol::strict_json(query.text(7));
    if (!arguments.is_object() || !result.is_object() || result.value("ok",Json()) != true ||
        result.value("effect",Json()) != "none" || !result.contains("output") || !result["output"].is_object())
        throw std::runtime_error("Build proposal receipt is invalid");
    Json row{{"task_id",task},{"action_id",action},{"workspace",query.text(0)},
        {"tool",query.text(1)},{"status",query.text(2)},{"effect",query.text(3)},
        {"arguments",std::move(arguments)},{"result",std::move(result)},{"decision",nullptr}};
    Statement decision(db_, R"(SELECT decision,digest,execution_task_id,execution_action_id
        FROM build_proposal_decisions WHERE source_action_id=?)");
    decision.bind(1,action);
    if (decision.next()) {
        for (int column = 0; column < 4; ++column)
            if (decision.bytes(column) > 256) throw std::runtime_error("Build decision metadata exceeds read size limit");
        row["decision"] = {{"decision",decision.text(0)},{"digest",decision.text(1)},
            {"execution_task_id",decision.text(2).empty() ? Json(nullptr) : Json(decision.text(2))},
            {"execution_action_id",decision.text(3).empty() ? Json(nullptr) : Json(decision.text(3))}};
    }
    return row;
}
Json Store::decide_build_proposal(const std::string& task, const std::string& action,
                                const std::string& digest, bool approve,
                                const std::string& expected_workspace) {
    if (digest.size() != 64 || !std::ranges::all_of(digest, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument("Build approval requires a lowercase SHA256 digest");
    Transaction tx(db_);
    auto proposal = build_proposal(task,action);
    if (proposal["workspace"] != expected_workspace ||
        proposal["result"]["output"].value("digest",Json()) != digest)
        throw std::runtime_error("Build proposal workspace or reviewed digest changed");
    const std::string chosen = approve ? "approved" : "rejected";
    if (!proposal["decision"].is_null()) {
        if (proposal["decision"]["decision"] != chosen || proposal["decision"]["digest"] != digest)
            throw std::runtime_error("Build proposal already has a conflicting decision");
        proposal["decision_created"] = false;
        tx.commit();
        return proposal;
    }
    if (has_running_tasks())
        throw std::runtime_error("Finish or recover running tasks before deciding a build proposal");
    Json decision{{"decision",chosen},{"digest",digest},{"execution_task_id",nullptr},{"execution_action_id",nullptr}};
    if (approve) {
        const auto execution_task = new_id(), execution_action = new_id();
        Statement create(db_, "INSERT INTO tasks(id,prompt,workspace,provider,status) VALUES(?,?,?,'approved-build','running')");
        create.bind(1,execution_task); create.bind(2,"Execute approved build proposal " + action);
        create.bind(3,expected_workspace); create.next();
        event(execution_task,"task.started",{{"provider","approved-build"},{"workspace",expected_workspace}});
        Statement prepare(db_, R"(INSERT INTO actions(id,task_id,call_id,tool,arguments,status)
            VALUES(?,?,'approved_build_1','build_workspace','{}','prepared'))");
        prepare.bind(1,execution_action); prepare.bind(2,execution_task); prepare.next();
        event(execution_task,"action.prepared",{{"action_id",execution_action},{"call_id","approved_build_1"},
            {"tool","build_workspace"},{"arguments",Json::object()}});
        decision["execution_task_id"] = execution_task;
        decision["execution_action_id"] = execution_action;
        Statement insert(db_, R"(INSERT INTO build_proposal_decisions
            (source_action_id,digest,decision,execution_task_id,execution_action_id) VALUES(?,?,'approved',?,?))");
        insert.bind(1,action); insert.bind(2,digest); insert.bind(3,execution_task); insert.bind(4,execution_action); insert.next();
    } else {
        Statement insert(db_, "INSERT INTO build_proposal_decisions(source_action_id,digest,decision) VALUES(?,?,'rejected')");
        insert.bind(1,action); insert.bind(2,digest); insert.next();
    }
    event(task,approve ? "build.proposal.approved" : "build.proposal.rejected",
        {{"action_id",action},{"decision",decision}});
    proposal["decision"] = std::move(decision);
    proposal["decision_created"] = true;
    tx.commit();
    return proposal;
}
Json Store::proposal_continuation(const std::string& task, const std::string& action) const {
    ReadSnapshot snapshot(db_);
    const auto proposal = build_proposal(task,action);
    const auto& output = proposal.at("result").at("output");
    if (!output.contains("digest") || !valid_digest(output.at("digest")))
        throw std::runtime_error("Build proposal has an invalid receipt digest");
    auto signed_output = output;
    signed_output.erase("digest");
    if (output.at("digest") != sha256_bytes(signed_output.dump()))
        throw std::runtime_error("Build proposal content differs from its reviewed digest");
    if (output.contains("workspace") && output.at("workspace") != proposal.at("workspace"))
        throw std::runtime_error("Build proposal receipt workspace is inconsistent");
    const auto& decision = proposal.at("decision");
    if (decision.is_null()) return {{"status","pending"},{"reason","Build proposal awaits approval"}};
    if (decision.at("digest") != output.at("digest"))
        throw std::runtime_error("Build decision digest differs from its proposal");
    Json execution = nullptr;
    if (decision.at("decision") == "rejected") {
        if (!decision.at("execution_task_id").is_null() || !decision.at("execution_action_id").is_null())
            throw std::runtime_error("Rejected build proposal has an execution association");
    } else if (decision.at("decision") == "approved") {
        if (!decision.at("execution_task_id").is_string() || !decision.at("execution_action_id").is_string())
            throw std::runtime_error("Approved build proposal has no execution association");
        const auto execution_task = decision.at("execution_task_id").get<std::string>();
        const auto execution_action = decision.at("execution_action_id").get<std::string>();
        if (execution_task.empty() || execution_action.empty() || execution_task == task || execution_action == action)
            throw std::runtime_error("Approved build execution association is inconsistent");
        Statement executed(db_, R"(SELECT t.workspace,t.provider,t.status,a.call_id,a.tool,a.status,a.effect,
            length(CAST(a.arguments AS BLOB)),length(CAST(a.result AS BLOB)),a.arguments,a.result
            FROM tasks t JOIN actions a ON a.task_id=t.id WHERE t.id=? AND a.id=?)");
        executed.bind(1,execution_task); executed.bind(2,execution_action);
        if (!executed.next()) throw std::runtime_error("Approved build execution association is missing");
        if (executed.bytes(0) > 32768 || executed.number(7) > 256 * 1024 ||
            executed.number(8) > static_cast<long long>(checkpoint_limit))
            throw std::runtime_error("Approved build continuation exceeds read size limit");
        for (int column = 1; column <= 6; ++column)
            if (executed.bytes(column) > 256) throw std::runtime_error("Approved build execution metadata exceeds read size limit");
        if (executed.text(0) != proposal.at("workspace").get<std::string>() || executed.text(1) != "approved-build" ||
            executed.text(3) != "approved_build_1" || executed.text(4) != "build_workspace" ||
            protocol::strict_json(executed.text(9)) != Json::object())
            throw std::runtime_error("Approved build execution identity is inconsistent");
        Statement count(db_, "SELECT count(*) FROM actions WHERE task_id=?");
        count.bind(1,execution_task); count.next();
        if (count.number(0) != 1) throw std::runtime_error("Approved build execution has an unexpected action ledger");
        const auto task_status = executed.text(2), action_status = executed.text(5), effect = executed.text(6);
        const std::array task_statuses{"running","completed","failed","cancelled","needs_attention","interrupted"};
        if (std::ranges::find(task_statuses,task_status) == task_statuses.end())
            throw std::runtime_error("Approved build execution has an invalid task status");
        if (action_status == "prepared" || action_status == "dispatched") {
            if (task_status != "running" || effect != (action_status == "prepared" ? "none" : "unknown"))
                throw std::runtime_error("Approved build execution intent is inconsistent");
            return {{"status","pending"},{"reason","Approved build is running or requires recovery"}};
        }
        if (action_status == "unknown" || effect == "unknown" || effect == "partial")
            return {{"status","blocked"},{"reason","Approved build effects require inspection; execution will not be replayed"}};
        if (effect != "none" && effect != "applied")
            throw std::runtime_error("Approved build execution has an invalid effect");
        Json body;
        if (action_status == "not_executed") {
            if (effect != "none" || task_status != "interrupted")
                throw std::runtime_error("Undispatched approved build recovery is inconsistent");
            body = {{"ok",false},{"output",Json::object()},
                    {"error","Interrupted before execution; the approved build was not retried"},{"effect","none"}};
        } else if (action_status == "succeeded" || action_status == "failed") {
            body = protocol::strict_json(executed.text(10));
            if (!body.is_object() || !body.contains("ok") || !body.at("ok").is_boolean() ||
                !body.contains("output") || !body.contains("error") || !body.at("error").is_string() ||
                !body.contains("effect") || body.at("effect") != effect || body.at("ok") != (action_status == "succeeded"))
                throw std::runtime_error("Approved build execution receipt is inconsistent");
            if ((task_status == "completed" && action_status != "succeeded") ||
                ((task_status == "failed" || task_status == "cancelled") && action_status != "failed") ||
                task_status == "needs_attention")
                throw std::runtime_error("Approved build task status differs from its receipt");
        } else throw std::runtime_error("Approved build execution has no terminal receipt");
        // A receipt can commit before finish_task. Recovery acknowledges the
        // interrupted executor without repeating the already recorded action.
        if (task_status == "running")
            return {{"status","pending"},{"reason","Approved build completion requires recovery"}};
        execution = {{"task_id",execution_task},{"action_id",execution_action},
                     {"status",action_status},{"result",std::move(body)}};
    } else throw std::runtime_error("Build proposal has an invalid decision");
    Json body{{"ok",true},{"output",{{"proposal",output},{"decision",decision},{"execution",std::move(execution)}}},
              {"error",""},{"effect","none"}};
    if (body.dump().size() > checkpoint_limit) throw std::runtime_error("Build continuation exceeds checkpoint size limit");
    return {{"status","ready"},{"result",std::move(body)}};
}

bool Store::consume_build_proposal(const std::string& task, const std::string& action,
                                  const Json& expected_result, const Json& next_checkpoint) {
    Transaction tx(db_);
    const auto continuation = proposal_continuation(task,action);
    if (continuation.at("status") != "ready" || continuation.at("result") != expected_result)
        throw std::runtime_error("Build proposal is not ready or its continuation result changed");
    if (proposal_was_consumed(db_,task,action)) {
        tx.commit();
        return false;
    }
    const auto saved = read_checkpoint(db_,task);
    const auto& state = saved.at("state");
    if (pending_proposal(db_,task,state) != action)
        throw std::runtime_error("Build proposal does not match the checkpoint cursor");
    if (!state.contains("messages") || !state.at("messages").is_array())
        throw std::runtime_error("Build proposal checkpoint has no conversation history");
    const auto& call = pending_call(state);
    const auto call_id = call.at("id").get<std::string>();
    for (const auto& message : state.at("messages"))
        if (message.is_object() && message.value("role", "") == "tool" && message.value("tool_call_id", "") == call_id)
            throw std::runtime_error("Build proposal already has a tool result without a consumption record");
    auto expected_checkpoint = state;
    expected_checkpoint["messages"].push_back({{"role","tool"},{"tool_call_id",call_id},{"content",expected_result.dump()}});
    expected_checkpoint["next_call"] = state.at("next_call").get<std::size_t>() + 1;
    if (next_checkpoint != expected_checkpoint)
        throw std::runtime_error("Build proposal consumption must advance exactly one tool result");
    save_checkpoint_in_transaction(db_,task,next_checkpoint,nullptr);
    event(task,"build.proposal.consumed",{{"action_id",action},{"call_id",call_id},
        {"digest",expected_result.at("output").at("proposal").at("digest")}});
    tx.commit();
    return true;
}
Json Store::recover() {
    Transaction tx(db_);
    Statement query(db_, "SELECT id FROM tasks WHERE status='running'");
    std::vector<std::string> ids; while(query.next()) ids.push_back(query.text(0));
    Json result = Json::array();
    for (const auto& id : ids) {
        Statement unknown(db_, "UPDATE actions SET status='unknown',effect='unknown' WHERE task_id=? AND status='dispatched'");
        unknown.bind(1,id); unknown.next();
        const auto unknown_count = sqlite3_changes(db_);
        Statement skipped(db_, "UPDATE actions SET status='not_executed' WHERE task_id=? AND status='prepared'");
        skipped.bind(1,id); skipped.next();
        Statement update(db_, "UPDATE tasks SET status='interrupted',summary='Interrupted run; inspect actions before starting new work',updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id=?");
        update.bind(1,id); update.next();
        Json data{{"task_id",id},{"unknown_actions",unknown_count},{"automatic_retry",false}};
        event(id,"task.recovered",data); result.push_back(data);
    }
    tx.commit(); return result;
}
}
