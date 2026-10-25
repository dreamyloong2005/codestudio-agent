#include "codestudio/state_reader.hpp"
#include "codestudio/store.hpp"
#include "../provider/protocol.hpp"
#include "scheduler_storage.hpp"
#include "cron_storage.hpp"
#include "supervisor_storage.hpp"
#include <sqlite3.h>
#include <array>
#include <climits>
#include <stdexcept>

namespace codestudio {
namespace {
constexpr std::size_t page_bytes = 2 * 1024 * 1024;
constexpr std::size_t text_bytes = 64 * 1024;
constexpr std::size_t metadata_bytes = 8 * 1024;
constexpr std::size_t payload_bytes = 256 * 1024;
constexpr std::size_t checkpoint_bytes = 16 * 1024 * 1024;

void sql(sqlite3* db, const char* statement) {
    char* error = nullptr;
    const auto result = sqlite3_exec(db, statement, nullptr, nullptr, &error);
    if (result == SQLITE_OK) return;
    const std::string message = error ? error : "State read failed";
    sqlite3_free(error);
    throw std::runtime_error(message);
}

class Snapshot {
public:
    explicit Snapshot(sqlite3* db) : db_(db) { sql(db_, "BEGIN"); }
    ~Snapshot() { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
private:
    sqlite3* db_;
};

class Statement {
public:
    Statement(sqlite3* db, const char* text) : db_(db) {
        if (sqlite3_prepare_v2(db_, text, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    void bind(int index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
            throw std::runtime_error("State cursor bind failed");
    }
    void bind(int index, const std::string& value) {
        if (value.size() > INT_MAX || sqlite3_bind_text(statement_, index, value.data(),
            static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("State identifier bind failed");
    }
    bool next() {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    std::int64_t number(int column) const { return sqlite3_column_int64(statement_, column); }
    bool integer(int column) const { return sqlite3_column_type(statement_,column)==SQLITE_INTEGER; }
    bool null(int column) const { return sqlite3_column_type(statement_,column)==SQLITE_NULL; }
    std::size_t bytes(int column) const { return static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)); }
    std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_, column);
        return value ? std::string(reinterpret_cast<const char*>(value), bytes(column)) : "";
    }
private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

void pagination(std::int64_t cursor, std::size_t limit) {
    if (cursor < 0) throw std::invalid_argument("State cursor must be nonnegative");
    if (!limit || limit > 100) throw std::invalid_argument("State page limit must be in 1..100");
}

void omitted(Json& row, const std::string& field, std::size_t size) {
    row[field] = nullptr;
    row[field + "_omitted"] = true;
    row[field + "_bytes"] = size;
}

void field(Json& row, const char* name, const Statement& query, int column, std::size_t maximum) {
    if (query.bytes(column) > maximum) omitted(row, name, query.bytes(column));
    else row[name] = query.text(column);
}

std::string identifier(const Statement& query, int column) {
    if (query.bytes(column) > 256) throw std::runtime_error("Stored task identifier is too long");
    return query.text(column);
}

Json task_row(const Statement& query) {
    Json row{{"id",identifier(query,0)}};
    field(row, "prompt", query, 1, text_bytes);
    field(row, "workspace", query, 2, metadata_bytes);
    field(row, "provider", query, 3, metadata_bytes);
    field(row, "status", query, 4, metadata_bytes);
    field(row, "summary", query, 5, text_bytes);
    field(row, "created_at", query, 6, metadata_bytes);
    field(row, "updated_at", query, 7, metadata_bytes);
    return row;
}

// Account for the envelope and commas before adding complete records. Every
// individual bounded record fits this budget, so pagination always advances.
bool append(Json& page, Json row, std::size_t limit, std::size_t& size) {
    const auto bytes = row.dump(-1, ' ', false, Json::error_handler_t::replace).size() + 1;
    if (page["items"].size() >= limit || size + bytes > page_bytes) {
        if (page["items"].empty()) throw std::runtime_error("State record exceeds page size");
        page["has_more"] = true;
        return false;
    }
    size += bytes;
    page["items"].push_back(std::move(row));
    return true;
}
}

struct StateReader::Impl {
    std::filesystem::path database;
    mutable sqlite3* db = nullptr;
    explicit Impl(std::filesystem::path path) : database(std::move(path)) {}
    ~Impl() { if (db) sqlite3_close(db); }

    bool open() const {
        if (!db) {
            std::error_code error;
            const auto exists = std::filesystem::exists(database, error);
            if (error) throw std::filesystem::filesystem_error("Cannot inspect state database", database, error);
            if (!exists) return false;
            if (sqlite3_open_v2(path_utf8(database).c_str(), &db,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
                const std::string message = db ? sqlite3_errmsg(db) : "Cannot open state database read-only";
                if (db) sqlite3_close(db);
                db = nullptr;
                throw std::runtime_error(message);
            }
            sqlite3_busy_timeout(db, 1000);
        }
        try {
            if (version() == 0) {
                // Store creates the file before committing its schema. Do not
                // cache that empty view or mistake it for an initialized DB.
                sqlite3_close(db); db = nullptr;
                return false;
            }
        }
        catch (...) { sqlite3_close(db); db = nullptr; throw; }
        return true;
    }

    int version() const {
        Statement query(db, "PRAGMA user_version");
        if (!query.next() || (query.number(0) != 0 && query.number(0) != 7))
            throw std::runtime_error("Unsupported state schema for read-only queries; create a new state database");
        const auto version = static_cast<int>(query.number(0));
        if (version == 0) {
            Statement objects(db, "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE name NOT GLOB 'sqlite_*')");
            if (!objects.next() || objects.number(0) != 0)
                throw std::runtime_error("Unversioned state database is not empty; create a new state database");
        }
        return version;
    }
};

StateReader::StateReader(const std::filesystem::path& database)
    : impl_(std::make_unique<Impl>(std::filesystem::absolute(database))) { impl_->open(); }
StateReader::~StateReader() = default;

Json StateReader::crons(std::int64_t before, std::size_t limit) const {
    pagination(before,limit);
    Json page{{"items",Json::array()},{"next_before",0},{"has_more",false}};
    if (!impl_->open()) return page;
    Snapshot snapshot(impl_->db);
    if (impl_->version()==0) return page;
    const auto text=std::string("SELECT ")+cron_storage::columns+
        " FROM cron_schedules WHERE (?=0 OR rowid<?) ORDER BY rowid DESC LIMIT ?";
    Statement query(impl_->db,text.c_str()); query.bind(1,before); query.bind(2,before);
    query.bind(3,static_cast<std::int64_t>(limit+1));
    std::size_t size=128;
    while (query.next()) {
        if (page["items"].size()==limit) { page["has_more"]=true; break; }
        if (!append(page,cron_storage::definition(query),limit,size)) break;
        page["next_before"]=query.number(cron_storage::cursor_column);
    }
    return page;
}
Json StateReader::cron(const std::string& id) const {
    scheduler_storage::identifier(id);
    if (!impl_->open()) throw std::runtime_error("Cron schedule not found");
    Snapshot snapshot(impl_->db);
    if (impl_->version()==0) throw std::runtime_error("Cron schedule not found");
    const auto text=std::string("SELECT ")+cron_storage::columns+" FROM cron_schedules WHERE id=?";
    Statement query(impl_->db,text.c_str()); query.bind(1,id);
    if (!query.next()) throw std::runtime_error("Cron schedule not found");
    return cron_storage::definition(query);
}

Json StateReader::schedules(std::int64_t before, std::size_t limit) const {
    pagination(before,limit);
    Json page{{"items",Json::array()},{"next_before",0},{"has_more",false}};
    if (!impl_->open()) return page;
    Snapshot snapshot(impl_->db);
    if (impl_->version()==0) return page;
    const auto text=std::string("SELECT ")+scheduler_storage::columns+
        " FROM scheduler_jobs WHERE (?=0 OR rowid<?) ORDER BY rowid DESC LIMIT ?";
    Statement query(impl_->db,text.c_str()); query.bind(1,before); query.bind(2,before);
    query.bind(3,static_cast<std::int64_t>(limit+1));
    std::size_t size=128;
    while (query.next()) {
        if (page["items"].size()==limit) { page["has_more"]=true; break; }
        if (!append(page,scheduler_storage::job(query),limit,size)) break;
        page["next_before"]=query.number(19);
    }
    return page;
}
Json StateReader::schedule(const std::string& id) const {
    scheduler_storage::identifier(id);
    if (!impl_->open()) throw std::runtime_error("Scheduled job not found");
    Snapshot snapshot(impl_->db);
    if (impl_->version()==0) throw std::runtime_error("Scheduled job not found");
    const auto text=std::string("SELECT ")+scheduler_storage::columns+" FROM scheduler_jobs WHERE id=?";
    Statement query(impl_->db,text.c_str()); query.bind(1,id);
    if (!query.next()) throw std::runtime_error("Scheduled job not found");
    return scheduler_storage::job(query);
}
Json StateReader::scheduler() const {
    if (!impl_->open()) return scheduler_storage::empty_control();
    Snapshot snapshot(impl_->db);
    if (impl_->version()==0) return scheduler_storage::empty_control();
    Statement query(impl_->db,"SELECT state,owner,pid,generation,stop_epoch,started_at_ms,heartbeat_at_ms FROM scheduler_control WHERE singleton=1");
    if (!query.next()) throw std::runtime_error("Scheduler control is missing");
    return scheduler_storage::control(query);
}

Json StateReader::supervisor() const {
    if (!impl_->open()) return nullptr;
    Snapshot snapshot(impl_->db);
    if (impl_->version()==0) return nullptr;
    const auto text=std::string("SELECT ")+supervisor_storage::columns+" FROM supervisor_runs ORDER BY rowid DESC LIMIT 1";
    Statement query(impl_->db,text.c_str());
    return query.next() ? supervisor_storage::run(query) : Json(nullptr);
}
Json StateReader::scheduler_snapshot() const {
    Json result{{"worker",scheduler_storage::empty_control()},{"supervisor",nullptr}};
    if (!impl_->open()) return result;
    Snapshot snapshot(impl_->db);
    if (impl_->version()==0) return result;
    Statement worker(impl_->db,"SELECT state,owner,pid,generation,stop_epoch,started_at_ms,heartbeat_at_ms FROM scheduler_control WHERE singleton=1");
    if (!worker.next()) throw std::runtime_error("Scheduler control is missing");
    result["worker"]=scheduler_storage::control(worker);
    const auto text=std::string("SELECT ")+supervisor_storage::columns+" FROM supervisor_runs ORDER BY rowid DESC LIMIT 1";
    Statement supervisor(impl_->db,text.c_str());
    if (supervisor.next()) result["supervisor"]=supervisor_storage::run(supervisor);
    return result;
}
Json StateReader::supervisor_events(std::int64_t after, std::size_t limit) const {
    pagination(after,limit);
    Json page{{"items",Json::array()},{"next_after",after},{"has_more",false}};
    if (!impl_->open()) return page;
    Snapshot snapshot(impl_->db);
    if (impl_->version()==0) return page;
    Statement query(impl_->db,"SELECT seq,run_id,kind,payload,created_at_ms FROM supervisor_events WHERE seq>? ORDER BY seq LIMIT ?");
    query.bind(1,after); query.bind(2,static_cast<std::int64_t>(limit+1));
    std::size_t size=128;
    while (query.next()) {
        if (page["items"].size()==limit) { page["has_more"]=true; break; }
        if (!query.integer(0) || !query.integer(4) || query.number(0)<=0 || query.number(4)<=0 ||
            query.bytes(1)>256 || query.bytes(2)>256 || query.bytes(3)>payload_bytes)
            throw std::runtime_error("Supervisor event exceeds read limits or has invalid metadata");
        const auto run_id=query.text(1), kind=query.text(2);
        scheduler_storage::identifier(run_id);
        const auto payload=protocol::strict_json(query.text(3));
        supervisor_storage::record(payload);
        if (!payload.is_object() || payload.value("id",Json())!=run_id ||
            payload.value("updated_at_ms",Json())!=query.number(4) ||
            (kind=="supervisor.child_exited" && (payload.at("worker_pid")!=0 || payload.at("last_exit_code").is_null() ||
                (payload.at("phase")!="starting" && payload.at("phase")!="running" && payload.at("phase")!="stopping"))) ||
            (kind!="supervisor.child_exited" &&
             (payload.value("phase",Json()).is_string() ? "supervisor."+payload.at("phase").get<std::string>() : "")!=kind))
            throw std::runtime_error("Invalid supervisor event payload");
        Json row{{"seq",query.number(0)},{"run_id",run_id},{"kind",kind},{"payload",payload},{"created_at_ms",query.number(4)}};
        if (!append(page,std::move(row),limit,size)) break;
        page["next_after"]=query.number(0);
    }
    return page;
}

Json StateReader::tasks(std::int64_t before, std::size_t limit) const {
    pagination(before, limit);
    Json page{{"items",Json::array()},{"next_before",0},{"has_more",false}};
    if (!impl_->open()) return page;
    Snapshot snapshot(impl_->db);
    if (impl_->version() == 0) return page;
    Statement query(impl_->db, R"(SELECT id,prompt,workspace,provider,status,summary,created_at,updated_at,rowid
        FROM tasks WHERE (?=0 OR rowid<?) ORDER BY rowid DESC LIMIT ?)" );
    query.bind(1,before); query.bind(2,before); query.bind(3,static_cast<std::int64_t>(limit + 1));
    std::size_t size = 128;
    while (query.next()) {
        if (page["items"].size() == limit) { page["has_more"] = true; break; }
        if (!append(page, task_row(query), limit, size)) break;
        page["next_before"] = query.number(8);
    }
    return page;
}

Json StateReader::events(std::int64_t after, std::size_t limit) const {
    pagination(after, limit);
    Json page{{"items",Json::array()},{"next_after",after},{"has_more",false}};
    if (!impl_->open()) return page;
    Snapshot snapshot(impl_->db);
    if (impl_->version() == 0) return page;
    Statement query(impl_->db, R"(SELECT seq,task_id,kind,payload,created_at,schema_version
        FROM events WHERE seq>? ORDER BY seq LIMIT ?)" );
    query.bind(1,after); query.bind(2,static_cast<std::int64_t>(limit + 1));
    std::size_t size = 128;
    while (query.next()) {
        if (page["items"].size() == limit) { page["has_more"] = true; break; }
        if (query.text(5) != "2") throw std::runtime_error("Unsupported event schema version");
        Json row{{"seq",query.number(0)},{"task_id",identifier(query,1)},
                 {"schema_version",query.number(5)}};
        field(row, "kind", query, 2, metadata_bytes);
        field(row, "created_at", query, 4, metadata_bytes);
        if (query.bytes(3) > payload_bytes) omitted(row, "payload", query.bytes(3));
        else row["payload"] = protocol::strict_json(query.text(3));
        if (!append(page, std::move(row), limit, size)) break;
        page["next_after"] = query.number(0);
    }
    return page;
}

Json StateReader::task(const std::string& id) const {
    if (id.empty() || id.size() > 256 || id.find('\0') != std::string::npos)
        throw std::invalid_argument("Task identifier must contain 1..256 bytes without NUL");
    if (!impl_->open()) throw std::runtime_error("Task not found");
    Snapshot snapshot(impl_->db);
    if (impl_->version() == 0) throw std::runtime_error("Task not found");
    Statement query(impl_->db, R"(SELECT id,prompt,workspace,provider,status,summary,created_at,updated_at
        FROM tasks WHERE id=?)" );
    query.bind(1,id);
    if (!query.next()) throw std::runtime_error("Task not found");
    auto row = task_row(query);
    Statement actions(impl_->db, "SELECT count(*) FROM actions WHERE task_id=?");
    actions.bind(1,id); actions.next();
    row["action_count"] = actions.number(0);
    row["checkpoint"] = nullptr;
    Statement saved(impl_->db, "SELECT version,state,binding FROM task_checkpoints WHERE task_id=?");
    saved.bind(1,id);
    if (!saved.next()) return row;
    if (saved.text(0) != "2") throw std::runtime_error("Unsupported task checkpoint version");
    if (saved.bytes(1) > checkpoint_bytes || saved.bytes(2) > checkpoint_bytes ||
        saved.bytes(1) + saved.bytes(2) > checkpoint_bytes)
        throw std::runtime_error("Task checkpoint exceeds read size limit");
    const auto state = protocol::strict_json(saved.text(1));
    const auto binding = protocol::strict_json(saved.text(2));
    if (!state.is_object() || !state.contains("version") || !state["version"].is_number_integer() || state["version"] != 2 ||
        !binding.is_object() || !binding.contains("version") || !binding["version"].is_number_integer() || binding["version"] != 2)
        throw std::runtime_error("Unsupported task checkpoint format version");
    if (!state.contains("phase") || !state["phase"].is_string() ||
        !state.contains("resumable") || !state["resumable"].is_boolean())
        throw std::runtime_error("Invalid task checkpoint summary");
    Json summary{{"version",2},{"resumable",state["resumable"]}};
    const auto& phase = state["phase"].get_ref<const std::string&>();
    if (phase != "ready" && phase != "model_pending" && phase != "tools" && phase != "finished")
        throw std::runtime_error("Invalid task checkpoint phase");
    summary["phase"] = phase;
    for (const auto* name : {"turns","max_turns"}) {
        if (state.contains(name) && state[name].is_number_integer()) summary[name] = state[name];
    }
    if (state.contains("blocked_reason") && state["blocked_reason"].is_string()) {
        const auto& reason = state["blocked_reason"].get_ref<const std::string&>();
        if (reason.size() > 1024) omitted(summary, "blocked_reason", reason.size());
        else summary["blocked_reason"] = reason;
    }
    row["checkpoint"] = std::move(summary);
    return row;
}
Json StateReader::build_proposal(const std::string& task, const std::string& action) const {
    for (const auto* id : {&task, &action})
        if (id->empty() || id->size() > 256 || id->find('\0') != std::string::npos)
            throw std::invalid_argument("Proposal identifiers must contain 1..256 bytes without NUL");
    if (!impl_->open()) throw std::runtime_error("Build proposal not found");
    Snapshot snapshot(impl_->db);
    if (impl_->version() == 0) throw std::runtime_error("Build proposal not found");
    Statement query(impl_->db, R"(SELECT t.workspace,a.tool,a.status,a.effect,a.arguments,a.result
        FROM actions a JOIN tasks t ON t.id=a.task_id WHERE a.task_id=? AND a.id=?)");
    query.bind(1,task); query.bind(2,action);
    if (!query.next()) throw std::runtime_error("Build proposal not found");
    if (query.bytes(0) > 32768 || query.bytes(1) > metadata_bytes || query.bytes(2) > metadata_bytes ||
        query.bytes(3) > metadata_bytes || query.bytes(4) > payload_bytes || query.bytes(5) > payload_bytes)
        throw std::runtime_error("Build proposal exceeds read size limit");
    if (query.text(1) != "propose_build" || query.text(2) != "succeeded" || query.text(3) != "none")
        throw std::runtime_error("Action is not a successful build proposal");
    auto arguments = protocol::strict_json(query.text(4));
    auto result = protocol::strict_json(query.text(5));
    if (!arguments.is_object() || !result.is_object() || result.value("ok",Json()) != true ||
        result.value("effect",Json()) != "none" || !result.contains("output") || !result["output"].is_object())
        throw std::runtime_error("Build proposal receipt is invalid");
    Json row{{"task_id",task},{"action_id",action},{"workspace",query.text(0)},
        {"tool",query.text(1)},{"status",query.text(2)},{"effect",query.text(3)},
        {"arguments",std::move(arguments)},{"result",std::move(result)},{"decision",nullptr}};
    Statement decision(impl_->db, R"(SELECT decision,digest,execution_task_id,execution_action_id
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
}
