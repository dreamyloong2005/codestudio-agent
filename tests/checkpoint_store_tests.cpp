#include "codestudio/store.hpp"
#include <sqlite3.h>
#include <fstream>
#include <stdexcept>

namespace {
using namespace codestudio;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F function, const char* message) {
    bool caught = false;
    try { function(); } catch (const std::exception&) { caught = true; }
    check(caught,message);
}
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("codestudio-checkpoints-" + new_id());
    Temp() { std::filesystem::create_directories(root); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root,error); }
};
class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        if (sqlite3_open(path_utf8(path).c_str(),&db_) != SQLITE_OK) {
            sqlite3_close(db_); db_ = nullptr;
            throw std::runtime_error("Checkpoint test database open failed");
        }
    }
    ~Database() { sqlite3_close(db_); }
    void exec(const std::string& text) {
        char* error = nullptr;
        const auto code = sqlite3_exec(db_,text.c_str(),nullptr,nullptr,&error);
        const std::string message = error ? error : "Checkpoint test SQL failed";
        sqlite3_free(error);
        if (code != SQLITE_OK) throw std::runtime_error(message);
    }
    std::string value(const char* text) {
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(db_,text,-1,&statement,nullptr) == SQLITE_OK,"Checkpoint test query prepare failed");
        const auto code = sqlite3_step(statement);
        std::string result;
        if (code == SQLITE_ROW) {
            const auto* value = sqlite3_column_text(statement,0);
            if (value) result = reinterpret_cast<const char*>(value);
        }
        sqlite3_finalize(statement);
        check(code == SQLITE_ROW,"Checkpoint test query returned no value");
        return result;
    }
private:
    sqlite3* db_ = nullptr;
};
Json state(const std::string& phase = "ready") {
    return {{"version",2},{"phase",phase},{"resumable",true},{"messages",Json::array({{{"role","user"},{"content","resume fixture"}}})},
        {"pending_calls",Json::array()},{"next_call",0},{"turns",1},{"max_turns",8},
        {"tool_failed",false},{"calls_seen",Json::array()},{"blocked_reason",""}};
}
Json binding() {
    return {{"version",2},{"provider","fixture"},{"workspace","fixture-workspace"},
        {"policy",{{"allow_write",false}}},{"tools",Json::array()},
        {"configuration",{{"model","fixture-model"},{"stream",true}}}};
}
std::string create(Store& store) { return store.create_task("resume fixture","fixture-workspace","fixture"); }
std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),{});
}

void initialization_and_durability() {
    Temp temp; const auto path = temp.root / "state.sqlite3";
    std::string id;
    const auto saved_state = state("model_pending");
    const auto saved_binding = binding();
    {
        Store store(path);
        id = create(store);
        check(store.task(id).at("checkpoint").is_null(),"New task acquired fabricated checkpoint");
        rejects([&] { store.checkpoint(id); },"New task has a checkpoint before saving");
        store.save_checkpoint(id,saved_state,saved_binding);
        store.finish_task(id,"cancelled","test interruption");
    }
    {
        Store store(path);
        const auto checkpoint = store.checkpoint(id);
        check(checkpoint.at("version") == 2 && checkpoint.at("state") == saved_state &&
            checkpoint.at("binding") == saved_binding,"Checkpoint failed reopen durability");
        const auto summary = store.task(id).at("checkpoint");
        check(summary.at("phase") == "model_pending" && summary.at("turns") == 1 &&
            !summary.contains("messages") && !summary.contains("binding"),"Task summary leaked full checkpoint");
        store.resume_task(id,saved_binding);
        check(store.task(id).at("status") == "running","Durable task did not resume");
    }
    Database database(path);
    check(database.value("PRAGMA user_version") == "7","Empty database was not initialized to the current schema");
}

void unsupported_schemas_are_read_only() {
    for (const int version : {0,1,2,3,4,5,6,999}) {
        Temp temp; const auto path = temp.root / "unsupported.sqlite3";
        {
            Database database(path);
            // sqliteXdata is a user table; a LIKE wildcard must not hide it as sqlite_* metadata.
            database.exec("CREATE TABLE sqliteXdata(value TEXT); INSERT INTO sqliteXdata VALUES('preserve'); PRAGMA user_version=" +
                std::to_string(version) + ";");
            check(database.value("PRAGMA journal_mode") == "delete","Unsupported schema fixture not in delete journal mode");
        }
        const auto before = bytes(path);
        rejects([&] { Store store(path); },"Unsupported or nonempty unversioned schema was accepted");
        check(bytes(path) == before,"Unsupported schema rejection mutated database bytes");
        check(!std::filesystem::exists(path_utf8(path) + "-wal") &&
              !std::filesystem::exists(path_utf8(path) + "-shm"),"Unsupported schema rejection enabled WAL");
        Database database(path);
        check(database.value("PRAGMA journal_mode") == "delete" &&
              database.value("PRAGMA user_version") == std::to_string(version),"Unsupported schema settings changed");
        check(database.value("SELECT value FROM sqliteXdata") == "preserve","Unsupported schema data changed");
    }
}

void validation_and_binding_guards() {
    Temp temp; Store store(temp.root / "state.sqlite3");
    const auto id = create(store);
    const auto original = state();
    const auto original_binding = binding();
    store.save_checkpoint(id,original,original_binding);
    auto changed_binding = original_binding;
    changed_binding["configuration"]["model"] = "different-model";
    rejects([&] { store.save_checkpoint(id,state("tools"),changed_binding); },"Checkpoint binding changed");
    check(store.checkpoint(id).at("state") == original,"Rejected binding changed checkpoint state");
    auto malformed = original;
    malformed.erase("phase");
    rejects([&] { store.save_checkpoint(id,malformed,original_binding); },"Missing checkpoint phase accepted");
    malformed = original; malformed["phase"] = "future_phase";
    rejects([&] { store.save_checkpoint(id,malformed,original_binding); },"Unknown checkpoint phase accepted");
    malformed = original; malformed["resumable"] = "yes";
    rejects([&] { store.save_checkpoint(id,malformed,original_binding); },"Invalid resumable flag accepted");
    malformed = original; malformed["messages"] = std::string(16 * 1024 * 1024,'x');
    rejects([&] { store.save_checkpoint(id,malformed,original_binding); },"Oversized checkpoint accepted");
    check(store.checkpoint(id).at("state") == original,"Rejected state replaced durable checkpoint");
    const auto missing_version_task = create(store);
    auto missing_version = original_binding; missing_version.erase("version");
    rejects([&] { store.save_checkpoint(missing_version_task,original,missing_version); },"Missing binding version accepted");
    rejects([&] { store.checkpoint(missing_version_task); },"Invalid initial checkpoint persisted");
    auto fractional_version = original_binding; fractional_version["version"] = 2.0;
    rejects([&] { store.save_checkpoint(id,original,fractional_version); },"Noninteger binding version accepted");
    const auto valid_task = store.task(id);
    const auto valid_events = store.events(id);
    const auto valid_checkpoint = store.checkpoint(id);
    const auto reject_version = [&](const Json& checkpoint_state,const Json& checkpoint_binding) {
        rejects([&] { store.save_checkpoint(id,checkpoint_state,checkpoint_binding); },"Unsupported checkpoint format was saved");
        check(store.task(id) == valid_task && store.events(id) == valid_events && store.checkpoint(id) == valid_checkpoint,
            "Rejected checkpoint format mutated task, checkpoint, or audit trail");
    };
    for (const auto& version : {Json(1),Json(3),Json(2.0),Json(2.5),Json(true),Json("2"),Json(nullptr)}) {
        auto invalid_state = original; invalid_state["version"] = version;
        reject_version(invalid_state,original_binding);
        auto invalid_binding = original_binding; invalid_binding["version"] = version;
        reject_version(original,invalid_binding);
    }
    auto unversioned_state = original; unversioned_state.erase("version");
    reject_version(unversioned_state,original_binding);
    reject_version(original,missing_version);
    auto long_reason = original;
    long_reason["blocked_reason"] = std::string(1023,'x') + "\xe4\xb8\xad";
    store.save_checkpoint(id,long_reason,original_binding);
    check(store.task(id).at("checkpoint").at("blocked_reason").get<std::string>().size() == 1023,
        "Checkpoint summary split UTF-8 at its size limit");
    check(!store.task(id).dump().empty(),"Bounded checkpoint summary is not valid JSON");
    store.save_checkpoint(id,original,original_binding);
    store.finish_task(id,"failed","interruption");
    const auto before = store.task(id);
    const auto events_before = store.events(id);
    rejects([&] { store.resume_task(id,changed_binding); },"Changed model binding resumed");
    check(store.task(id) == before && store.events(id) == events_before,"Rejected resume mutated task or audit trail");
    rejects([&] { store.save_checkpoint(id,original,original_binding); },"Terminal task checkpoint changed");
    store.resume_task(id,original_binding);
    const auto resumed_events = store.events(id);
    check(resumed_events.size() == events_before.size()+1 && resumed_events.back().at("kind") == "task.resumed",
        "Resume transition missing audit event");
    check(resumed_events.back().at("payload").at("previous_status") == "failed","Resume event omitted prior state");
    rejects([&] { store.resume_task(id,original_binding); },"Running task resumed twice");
    check(store.events(id) == resumed_events,"Duplicate resume changed audit trail");
}

void unsupported_checkpoint_versions_are_read_only() {
    Temp temp; const auto path = temp.root / "state.sqlite3"; Store store(path);
    const auto id = create(store);
    const auto saved_state = state();
    const auto saved_binding = binding();
    store.save_checkpoint(id,saved_state,saved_binding);
    store.finish_task(id,"failed","interruption");
    Database database(path);
    constexpr auto task_snapshot = "SELECT quote(status)||quote(summary)||quote(updated_at) FROM tasks";
    constexpr auto checkpoint_snapshot = "SELECT quote(version)||quote(state)||quote(binding)||quote(updated_at) FROM task_checkpoints";
    const auto original_task = database.value(task_snapshot);
    const auto original_events = store.events(id);
    const auto reject_stored_version = [&] {
        const auto original_checkpoint = database.value(checkpoint_snapshot);
        rejects([&] { (void)store.checkpoint(id); },"Unsupported checkpoint format was read");
        rejects([&] { (void)store.task(id); },"Unsupported checkpoint format was summarized");
        rejects([&] { store.resume_task(id,saved_binding); },"Unsupported checkpoint format resumed");
        check(database.value(task_snapshot) == original_task && store.events(id) == original_events &&
              database.value(checkpoint_snapshot) == original_checkpoint,
            "Rejected checkpoint format changed durable task, checkpoint, or audit trail");
    };
    for (const auto* version : {"1","3","2.5"}) {
        database.exec(std::string("UPDATE task_checkpoints SET version=") + version);
        reject_stored_version();
    }
    database.exec("UPDATE task_checkpoints SET version=2");
    for (const auto* field : {"state","binding"}) {
        const bool is_state = std::string(field) == "state";
        for (const auto& version : {Json(1),Json(3),Json(2.0),Json(2.5),Json(true),Json("2"),Json(nullptr)}) {
            auto changed = is_state ? saved_state : saved_binding;
            if (version.is_null()) changed.erase("version");
            else changed["version"] = version;
            database.exec(std::string("UPDATE task_checkpoints SET ") + field + "='" + changed.dump() + "'");
            reject_stored_version();
        }
        database.exec(std::string("UPDATE task_checkpoints SET ") + field + "='" +
            (is_state ? saved_state : saved_binding).dump() + "'");
    }
    check(store.checkpoint(id).at("state") == saved_state && store.checkpoint(id).at("binding") == saved_binding,
        "Current checkpoint did not remain readable after rejected formats");
}

void resume_status_and_effect_guards() {
    Temp temp; const auto path = temp.root / "state.sqlite3"; Store store(path);
    const auto configuration = binding();
    for (const auto* status : {"cancelled", "failed", "exhausted"}) {
        const auto id = create(store);
        store.save_checkpoint(id,state(),configuration);
        store.finish_task(id,status,"interrupted");
        store.resume_task(id,configuration);
        check(store.task(id).at("status") == "running","Eligible terminal status did not resume");
        store.finish_task(id,"completed","done");
    }
    for (const auto* status : {"completed", "needs_attention"}) {
        const auto id = create(store);
        store.save_checkpoint(id,state(),configuration);
        store.finish_task(id,status,"done");
        rejects([&] { store.resume_task(id,configuration); },"Final task resumed");
    }
    for (bool finished : {false,true}) {
        const auto id = create(store);
        auto checkpoint = state(finished ? "finished" : "ready");
        checkpoint["resumable"] = finished;
        store.save_checkpoint(id,checkpoint,configuration);
        store.finish_task(id,"failed","stopped");
        rejects([&] { store.resume_task(id,configuration); },"Non-resumable or finished checkpoint resumed");
    }
    const auto prepared = create(store);
    store.save_checkpoint(prepared,state("tools"),configuration);
    store.prepare_action(prepared,{"skipped","read_file",{{"path","x"}}});
    store.recover();
    check(store.task(prepared).at("actions")[0].at("status") == "not_executed","Prepared action recovery changed");
    store.resume_task(prepared,configuration);
    store.finish_task(prepared,"completed","done");
    for (const auto* effect : {"partial", "unknown"}) {
        const auto id = create(store);
        store.save_checkpoint(id,state("tools"),configuration);
        const auto action = store.prepare_action(id,{"uncertain","write_file",{{"path","x"},{"content","x"}}});
        store.dispatch_action(action);
        store.finish_action(action,{false,Json::object(),"uncertain",effect});
        store.finish_task(id,"failed","uncertain");
        const auto before = store.events(id);
        rejects([&] { store.resume_task(id,configuration); },"Uncertain action resumed");
        check(store.task(id).at("status") == "failed" && store.events(id) == before,
            "Rejected uncertain resume changed state");
    }
    for (const auto* status : {"prepared", "dispatched", "unknown"}) {
        const auto id = create(store);
        store.save_checkpoint(id,state("tools"),configuration);
        store.deny_action(id,{"stale","read_file",{{"path","x"}}},"fixture");
        store.finish_task(id,"cancelled","stopped");
        Database raw(path);
        raw.exec("UPDATE actions SET status='" + std::string(status) + "',effect='none' WHERE task_id='" + id + "'");
        rejects([&] { store.resume_task(id,configuration); },"Unresolved ledger status resumed");
    }
    const auto incompatible = create(store);
    store.save_checkpoint(incompatible,state(),configuration);
    store.finish_task(incompatible,"failed","stopped");
    {
        Database raw(path);
        raw.exec("UPDATE task_checkpoints SET version=3 WHERE task_id='" + incompatible + "'");
    }
    rejects([&] { store.resume_task(incompatible,configuration); },"Future checkpoint version resumed");
}

void atomic_receipts_and_completion() {
    Temp temp; const auto path = temp.root / "state.sqlite3"; std::string id;
    const auto configuration = binding();
    auto original = state("tools"); original["next_call"] = 0;
    auto advanced = original; advanced["next_call"] = 1;
    const Json invalid{{"phase","tools"}};
    {
        Store store(path); id = create(store); store.save_checkpoint(id,original,configuration);
        const auto action = store.prepare_action(id,{"read","read_file",{{"path","x"}}});
        store.dispatch_action(action);
        const auto before = store.events(id);
        rejects([&] { store.finish_action(action,{true,{{"content","value"}},"","none"},&invalid); },
            "Action accepted invalid checkpoint");
        check(store.task(id).at("actions")[0].at("status") == "dispatched" &&
            store.checkpoint(id).at("state") == original && store.events(id) == before,
            "Invalid checkpoint did not roll back action receipt");
        for (const auto& version : {Json(1),Json(3),Json(2.0),Json(2.5)}) {
            auto invalid_version = advanced; invalid_version["version"] = version;
            rejects([&] { store.finish_action(action,{true,{{"content","value"}},"","none"},&invalid_version); },
                "Action accepted unsupported checkpoint version");
            check(store.task(id).at("actions")[0].at("status") == "dispatched" &&
                store.checkpoint(id).at("state") == original && store.events(id) == before,
                "Unsupported checkpoint version did not roll back action receipt");
        }
        store.finish_action(action,{true,{{"content","value"}},"","none"},&advanced);
        check(store.task(id).at("actions")[0].at("status") == "succeeded" &&
            store.checkpoint(id).at("state").at("next_call") == 1,"Receipt and checkpoint did not advance together");
        const auto before_denial = store.events(id);
        const ToolCall denied{"denied","write_file",{{"path","x"},{"content","x"}}};
        rejects([&] { store.deny_action(id,denied,"permission denied",&invalid); },"Denial accepted invalid checkpoint");
        check(store.task(id).at("actions").size() == 1 && store.events(id) == before_denial,
            "Invalid checkpoint did not roll back denial");
        advanced["next_call"] = 2;
        store.deny_action(id,denied,"permission denied",&advanced);
        check(store.task(id).at("actions").size() == 2 && store.checkpoint(id).at("state").at("next_call") == 2,
            "Denial and checkpoint did not advance together");
        const auto before_finish = store.events(id);
        rejects([&] { store.finish_task(id,"completed","done",&invalid); },"Task finish accepted invalid checkpoint");
        check(store.task(id).at("status") == "running" && store.events(id) == before_finish,
            "Invalid checkpoint did not roll back task finish");
        advanced["phase"] = "finished";
        store.finish_task(id,"needs_attention","denied tool",&advanced);
    }
    {
        Store store(path);
        check(store.task(id).at("status") == "needs_attention" &&
            store.checkpoint(id).at("state") == advanced && store.task(id).at("actions").size() == 2,
            "Atomic final checkpoint or receipts not durable");
    }
}
}

void run_checkpoint_store_tests() {
    initialization_and_durability();
    unsupported_schemas_are_read_only();
    validation_and_binding_guards();
    unsupported_checkpoint_versions_are_read_only();
    resume_status_and_effect_guards();
    atomic_receipts_and_completion();
}
