#include "codestudio/state_reader.hpp"
#include "codestudio/store.hpp"
#include "codestudio/digest.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace {
using namespace codestudio;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("State reader: ") + message);
}
template<class F> void rejects(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("codestudio-state-reader-" + new_id());
    Temp() { std::filesystem::create_directories(root); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root,error); }
};
class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        if (sqlite3_open(path_utf8(path).c_str(),&db_) != SQLITE_OK) {
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("State reader test SQLite open failed");
        }
    }
    ~Database() { sqlite3_close(db_); }
    void exec(const std::string& text) {
        char* error = nullptr;
        const auto result = sqlite3_exec(db_,text.c_str(),nullptr,nullptr,&error);
        const std::string message = error ? error : "State reader fixture SQL failed";
        sqlite3_free(error);
        if (result != SQLITE_OK) throw std::runtime_error(message);
    }
    std::string value(const char* text) {
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(db_,text,-1,&statement,nullptr)==SQLITE_OK,"fixture query prepare");
        const auto result = sqlite3_step(statement);
        std::string value;
        if (result == SQLITE_ROW && sqlite3_column_text(statement,0))
            value = reinterpret_cast<const char*>(sqlite3_column_text(statement,0));
        sqlite3_finalize(statement);
        check(result == SQLITE_ROW,"fixture query row");
        return value;
    }
private:
    sqlite3* db_ = nullptr;
};
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),{}};
}

void absent_state_and_validation() {
    Temp temp;
    const auto path = temp.root / "absent" / "state.sqlite3";
    StateReader reader(path);
    check(reader.tasks()==Json{{"items",Json::array()},{"next_before",0},{"has_more",false}},"missing task page");
    check(reader.events(91)==Json{{"items",Json::array()},{"next_after",91},{"has_more",false}},"missing event page preserves cursor");
    check(reader.supervisor().is_null() && reader.scheduler_snapshot()["supervisor"].is_null() &&
        reader.scheduler_snapshot()["worker"]==reader.scheduler(),"missing supervisor view invented state");
    check(reader.supervisor_events(91)==Json{{"items",Json::array()},{"next_after",91},{"has_more",false}},
        "missing supervisor event page reset cursor");
    rejects([&] { reader.task("missing"); },"missing task accepted");
    rejects([&] { reader.build_proposal("missing","missing"); },"missing proposal accepted");
    check(!std::filesystem::exists(path.parent_path()),"reader created missing parent or state");
    for (const auto limit : {std::size_t(0),std::size_t(101),std::numeric_limits<std::size_t>::max()}) {
        rejects([&] { reader.tasks(0,limit); },"invalid task page limit");
        rejects([&] { reader.events(0,limit); },"invalid event page limit");
        rejects([&] { reader.supervisor_events(0,limit); },"invalid supervisor event page limit");
    }
    rejects([&] { reader.tasks(-1); },"negative task cursor");
    rejects([&] { reader.events(-1); },"negative event cursor");
    rejects([&] { reader.supervisor_events(-1); },"negative supervisor event cursor");
    rejects([&] { reader.task(""); },"empty task identifier");
    rejects([&] { reader.task(std::string(257,'x')); },"oversized task identifier");
    rejects([&] { reader.task(std::string("a\0b",3)); },"NUL task identifier");
    Store store(path);
    const auto id = store.create_task("created after reader",path_utf8(temp.root),"fixture");
    check(reader.tasks()["items"].size()==1 && reader.task(id)["id"]==id,"absent reader failed to notice first task");
}

void committed_pages() {
    Temp temp; const auto path = temp.root / "state.sqlite3";
    Store store(path); StateReader reader(path);
    std::vector<std::string> expected_tasks;
    std::vector<std::int64_t> expected_events;
    for (int index=0;index<7;++index) {
        const auto id = store.create_task("task " + std::to_string(index),path_utf8(temp.root),"fixture");
        store.finish_task(id,"completed","done");
        expected_tasks.push_back(id);
        for (const auto& event : store.events(id)) expected_events.push_back(event["seq"].get<std::int64_t>());
    }
    std::ranges::reverse(expected_tasks);
    std::vector<std::string> actual_tasks;
    std::int64_t before=0;
    for (;;) {
        const auto page=reader.tasks(before,2);
        for (const auto& row : page["items"]) actual_tasks.push_back(row["id"].get<std::string>());
        if (!page["has_more"].get<bool>()) break;
        const auto next=page["next_before"].get<std::int64_t>();
        check(next>0 && (!before || next<before),"task cursor did not advance"); before=next;
    }
    check(actual_tasks==expected_tasks,"task pagination lost, duplicated or reordered rows");
    std::vector<std::int64_t> actual_events;
    std::int64_t after=0;
    for (;;) {
        const auto page=reader.events(after,3);
        for (const auto& row : page["items"]) {
            check(row.contains("task_id") && row.contains("schema_version"),"event identity missing");
            actual_events.push_back(row["seq"].get<std::int64_t>());
        }
        const auto next=page["next_after"].get<std::int64_t>();
        check(next>=after,"event cursor decreased"); after=next;
        if (!page["has_more"].get<bool>()) break;
    }
    check(actual_events==expected_events,"event pagination lost, duplicated or reordered rows");
    check(reader.events(after)["items"].empty(),"event tail repeated last event");
    check(reader.events(after)["next_after"]==after,"empty tail reset event cursor");
    Database writer(path);
    writer.exec("BEGIN IMMEDIATE; INSERT INTO events(task_id,kind,payload) VALUES('" + expected_tasks.front() +
        "','uncommitted','{\"visible\":false}'); UPDATE tasks SET summary='pending' WHERE id='" + expected_tasks.front() + "';");
    check(reader.events(after)["items"].empty(),"uncommitted event leaked to reader");
    check(reader.task(expected_tasks.front())["summary"]=="done","uncommitted task change leaked");
    writer.exec("COMMIT");
    check(reader.events(after)["items"].size()==1,"reader did not observe committed event");
    check(reader.task(expected_tasks.front())["summary"]=="pending","reader retained stale transaction snapshot");
}

void uninitialized_state_reopens() {
    Temp temp; const auto path=temp.root / "initializing.sqlite3";
    Database initializing(path);
    StateReader reader(path);
    check(reader.tasks()["items"].empty() && reader.events()["items"].empty(),"schema 0 file was treated as initialized");
    check(reader.supervisor().is_null() && reader.supervisor_events()["items"].empty() &&
        reader.scheduler_snapshot()["supervisor"].is_null(),"schema 0 supervisor view was treated as initialized");
    rejects([&] { reader.task("missing"); },"uninitialized database returned a task");
    rejects([&] { reader.build_proposal("missing","missing"); },"uninitialized database returned a proposal");
    initializing.exec("BEGIN IMMEDIATE; CREATE TABLE initializing_only(value TEXT); PRAGMA user_version=7;");
    check(reader.tasks()["items"].empty() && reader.events()["items"].empty(),"uncommitted schema became observable");
    initializing.exec("ROLLBACK");
    {
        Store store(path);
        const auto id=store.create_task("initialized later",path_utf8(temp.root),"fixture");
        check(reader.task(id)["id"]==id && reader.events()["items"].size()==1,
            "persistent reader failed to reopen after schema initialization");
    }
}

void unsupported_schemas_are_rejected_without_changes() {
    Temp temp;
    for (const auto version : {0,1,2,3,4,5,6,999}) {
        const auto path=temp.root / ("unsupported-" + std::to_string(version) + ".sqlite3");
        {
            Database db(path);
            // sqliteXdata is a user table, even though LIKE 'sqlite_%' would match it.
            db.exec("CREATE TABLE sqliteXdata(value TEXT); INSERT INTO sqliteXdata VALUES('preserve'); "
                    "PRAGMA user_version=" + std::to_string(version) + ";");
        }
        const auto original=bytes(path);
        rejects([&] { StateReader reader(path); },"unsupported or nonempty unversioned state accepted");
        check(bytes(path)==original,"unsupported state read changed original bytes");
        check(!std::filesystem::exists(path_utf8(path)+"-wal") && !std::filesystem::exists(path_utf8(path)+"-shm"),
              "unsupported state read changed journal mode");
        Database db(path);
        check(db.value("PRAGMA journal_mode")=="delete" &&
              db.value("PRAGMA user_version")==std::to_string(version),"unsupported state settings changed");
        check(db.value("SELECT value FROM sqliteXdata")=="preserve","unsupported state contents changed");
    }
}

void supervisor_committed_reads_and_bounds() {
    Temp temp; const auto path=temp.root/"state.sqlite3";
    Store store(path); StateReader reader(path); Database raw(path);
    store.start_supervisor({"reader-run","reader-owner","workspace","process-identity",123,100,2,1000},{0,0},1000);
    const auto original=reader.supervisor(), original_events=reader.supervisor_events();
    raw.exec("BEGIN IMMEDIATE; UPDATE supervisor_runs SET heartbeat_at_ms=1001,updated_at_ms=1001; UPDATE scheduler_control SET stop_epoch=1;");
    check(reader.supervisor()==original && reader.supervisor_events()==original_events &&
        reader.scheduler_snapshot()==Json{{"worker",store.scheduler()},{"supervisor",original}},
        "reader observed an uncommitted supervisor or mixed scheduler snapshot");
    raw.exec("COMMIT");
    check(reader.scheduler_snapshot()["worker"]["stop_epoch"]==1 && reader.supervisor()["heartbeat_at_ms"]==1001,
        "reader kept a stale supervisor snapshot");
    raw.exec("UPDATE supervisor_runs SET max_restarts=1.5;");
    rejects([&] { reader.supervisor(); },"noninteger supervisor option accepted");
    rejects([&] { reader.scheduler_snapshot(); },"coherent snapshot accepted corrupt supervisor metadata");
    raw.exec("UPDATE supervisor_runs SET max_restarts=2;");
    const auto payload=original_events["items"][0]["payload"];
    const auto check_bad=[&](const Json& changed) {
        raw.exec("UPDATE supervisor_events SET payload='"+changed.dump()+"'");
        rejects([&] { reader.supervisor_events(); },"corrupt supervisor event payload accepted");
        raw.exec("UPDATE supervisor_events SET payload='"+payload.dump()+"'");
    };
    auto bad=payload; bad.erase("pid"); check_bad(bad);
    bad=payload; bad["extra"]=true; check_bad(bad);
    bad=payload; bad["id"]="wrong-run"; check_bad(bad);
    bad=payload; bad["pid"]=1.5; check_bad(bad);
    bad=payload; bad["pid"]="123"; check_bad(bad);
    bad=payload; bad["max_restarts"]=101; check_bad(bad);
    bad=payload; bad["generation"]=std::numeric_limits<std::uint64_t>::max(); check_bad(bad);
    bad=payload; bad["last_exit_code"]=-1; check_bad(bad);
    bad=payload; bad["last_exit_code"]=true; check_bad(bad);
    bad=payload; bad["phase"]="future-phase"; check_bad(bad);
    bad=payload; bad["workspace"]=std::string(32769,'x'); check_bad(bad);
    bad=payload; bad["updated_at_ms"]=1002; check_bad(bad);
    raw.exec("UPDATE supervisor_events SET kind='supervisor.unknown'");
    rejects([&] { reader.supervisor_events(); },"unknown supervisor event kind accepted");
    raw.exec("UPDATE supervisor_events SET kind='supervisor.child_exited'");
    rejects([&] { reader.supervisor_events(); },"unobserved exit event accepted");
    raw.exec("UPDATE supervisor_events SET kind='supervisor.starting',payload='"+std::string(256*1024+1,'x')+"'");
    rejects([&] { reader.supervisor_events(); },"oversized supervisor event was returned or truncated");
    raw.exec("UPDATE supervisor_events SET payload='"+payload.dump()+"'");
    for (int i=0;i<12;++i) {
        store.start_supervisor({"page-run-"+std::to_string(i),"page-owner",std::string(32768,'\x01'),"identity",123,100,2,1000},
            {1,0},1002+i);
    }
    std::int64_t cursor=0; std::size_t seen=0; bool byte_limited=false;
    for (;;) {
        const auto page=reader.supervisor_events(cursor,100);
        check(page.dump().size()<=2*1024*1024,"supervisor event page exceeded serialized size limit");
        if (page["has_more"]==true && page["items"].size()<100) byte_limited=true;
        seen+=page["items"].size(); cursor=page["next_after"].get<std::int64_t>();
        if (!page["has_more"].get<bool>()) break;
    }
    check(byte_limited && seen==25,"bounded supervisor pages lost records or missed byte limit");
}

void summaries_and_size_bounds() {
    Temp temp; const auto path=temp.root / "state.sqlite3";
    Store store(path); StateReader reader(path);
    const auto id=store.create_task("bounded reads",path_utf8(temp.root),"fixture");
    const Json state{{"version",2},{"phase","model_pending"},{"resumable",true},{"turns",2},{"max_turns",8},
        {"messages",Json::array({{{"role","user"},{"content","private-history-sentinel"}}})},
        {"blocked_reason",std::string(1025,'b')}};
    store.save_checkpoint(id,state,{{"version",2},{"private_binding","private-binding-sentinel"}});
    store.deny_action(id,{"call","write_file",{{"path","a"}}},"no grant");
    const auto row=reader.task(id);
    check(row["action_count"]==1 && !row.contains("actions"),"task actions were not summarized");
    check(row["checkpoint"]["phase"]=="model_pending" && row["checkpoint"]["turns"]==2,"checkpoint summary missing");
    check(row["checkpoint"]["blocked_reason"].is_null() && row["checkpoint"]["blocked_reason_omitted"]==true,
        "oversized checkpoint reason was silently cut");
    check(row.dump().find("private-history-sentinel")==std::string::npos &&
        row.dump().find("private-binding-sentinel")==std::string::npos,"task detail leaked raw checkpoint data");
    store.event(id,"oversized",{{"text",std::string(300*1024,'x')}});
    for (int index=0;index<20;++index) store.event(id,"large",{{"text",std::string(240*1024,'x')}});
    store.finish_task(id,"failed",std::string(70*1024,'s'));
    const auto detail=reader.task(id);
    check(detail["summary"].is_null() && detail["summary_omitted"]==true && detail["summary_bytes"]==70*1024,
        "oversized task summary was not explicitly omitted");
    const auto expected=store.events(id);
    std::vector<std::int64_t> sequences;
    std::int64_t cursor=0; bool byte_limited=false, omitted_payload=false;
    do {
        const auto page=reader.events(cursor,100);
        check(page.dump().size()<=2*1024*1024,"event page exceeded 2 MiB budget");
        if (page["has_more"]==true && page["items"].size()<100) byte_limited=true;
        for (const auto& event:page["items"]) {
            sequences.push_back(event["seq"].get<std::int64_t>());
            if (event["kind"]=="oversized") {
                check(event["payload"].is_null() && event["payload_omitted"]==true && event["payload_bytes"].get<int>()>256*1024,
                    "oversized payload not explicitly omitted");
                omitted_payload=true;
            }
        }
        cursor=page["next_after"].get<std::int64_t>();
        if (!page["has_more"].get<bool>()) break;
    } while (true);
    check(byte_limited && omitted_payload,"size-bound tests missed their boundary");
    check(sequences.size()==expected.size(),"bounded pages silently skipped event records");
    for (std::size_t index=0;index<sequences.size();++index)
        check(sequences[index]==expected[index]["seq"].get<std::int64_t>(),"bounded event cursor lost ordering");

    // JSON escaping can expand text sixfold. Exercise the serialized-byte
    // limit rather than assuming the raw SQLite text limit bounds a page.
    std::vector<std::string> task_ids{id};
    for (int index=0;index<8;++index)
        task_ids.push_back(store.create_task(std::string(64*1024,'\x01'),path_utf8(temp.root),"fixture"));
    std::ranges::reverse(task_ids);
    std::vector<std::string> listed;
    std::int64_t before=0; bool task_byte_limited=false;
    for (;;) {
        const auto page=reader.tasks(before,100);
        check(page.dump().size()<=2*1024*1024,"task page exceeded serialized 2 MiB budget");
        if (page["has_more"]==true && page["items"].size()<100) task_byte_limited=true;
        for (const auto& task:page["items"]) listed.push_back(task["id"].get<std::string>());
        before=page["next_before"].get<std::int64_t>();
        if (!page["has_more"].get<bool>()) break;
    }
    check(task_byte_limited && listed==task_ids,"bounded task pages lost or duplicated records");
}
#ifdef _WIN32
void proposal_reads_are_bounded_and_committed() {
    Temp temp; const auto path=temp.root / "state.sqlite3";
    Store store(path); StateReader reader(path); Database writer(path);
    const auto task=store.create_task("review only",path_utf8(temp.root),"fixture");
    const auto action=store.prepare_action(task,{"propose_1","propose_build",{{"manifest",{{"version",2}}},{"reason","review"}}});
    rejects([&]{ reader.build_proposal(task,action); },"prepared source accepted as a proposal");
    store.dispatch_action(action);
    rejects([&]{ reader.build_proposal(task,action); },"dispatched source accepted as a proposal");
    Json proposal{{"manifest",{{"version",2}}}};
    proposal["digest"]=sha256_bytes(proposal.dump());
    store.finish_action(action,{true,proposal,"","none"});
    const auto before=reader.build_proposal(task,action);
    check(before==store.build_proposal(task,action) && before["decision"].is_null(),"exact proposal read differs");
    writer.exec("BEGIN IMMEDIATE; UPDATE actions SET effect='unknown' WHERE id='" + action + "';");
    check(reader.build_proposal(task,action)==before,"reader observed uncommitted proposal mutation");
    writer.exec("ROLLBACK");
    store.finish_task(task,"completed","proposal ready");
    for (const auto& id : {std::string(),std::string(257,'x'),std::string("a\0b",3)}) {
        rejects([&]{ reader.build_proposal(id,action); },"invalid proposal task identifier accepted");
        rejects([&]{ reader.build_proposal(task,id); },"invalid proposal action identifier accepted");
    }
    const auto argument=before["arguments"].dump();
    writer.exec("UPDATE actions SET arguments='" + std::string(256*1024+1,'x') + "' WHERE id='" + action + "'");
    rejects([&]{ reader.build_proposal(task,action); },"oversized proposal arguments returned");
    writer.exec("UPDATE actions SET arguments='" + argument + "',result='" + std::string(256*1024+1,'x') + "' WHERE id='" + action + "'");
    rejects([&]{ reader.build_proposal(task,action); },"oversized proposal receipt returned");
}
#endif
void current_schema_and_checkpoint_versions_are_required() {
    Temp temp; const auto path=temp.root / "current.sqlite3";
    std::string task,action;
    const Json state{{"version",2},{"phase","finished"},{"resumable",false}};
    const Json binding{{"version",2}};
    {
        Store store(path);
        task=store.create_task("current record",path_utf8(temp.root),"fixture");
        store.save_checkpoint(task,state,binding);
#ifdef _WIN32
        action=store.prepare_action(task,{"proposal","propose_build",{{"manifest",{{"version",2}}}}});
        store.dispatch_action(action);
        Json proposal{{"manifest",{{"version",2}}}};
        proposal["digest"]=sha256_bytes(proposal.dump());
        store.finish_action(action,{true,proposal,"","none"});
#endif
        store.finish_task(task,"completed","ready");
    }
    {
        Database db(path);
        check(db.value("PRAGMA user_version")=="7","new state did not use current schema");
        db.exec("PRAGMA wal_checkpoint(TRUNCATE); PRAGMA journal_mode=DELETE;");
    }
    StateReader reader(path);
    check(reader.task(task)["checkpoint"]["version"]==2,"current checkpoint summary version changed");
#ifdef _WIN32
    check(reader.build_proposal(task,action)["action_id"]==action,"current proposal became unreadable");
#endif
    for (const auto* version : {"1","3","2.5"}) {
        {
            Database db(path);
            db.exec(std::string("UPDATE task_checkpoints SET version=") + version);
        }
        const auto original=bytes(path);
        rejects([&] { (void)reader.task(task); },"unsupported checkpoint envelope accepted");
        check(bytes(path)==original,"checkpoint refusal rewrote stored records");
    }
    {
        Database db(path);
        db.exec("UPDATE task_checkpoints SET version=2;");
    }
    for (const auto* field : {"state","binding"}) {
        for (const auto& version : {Json(1),Json(3),Json(2.0),Json(2.5),Json(true),Json("2"),Json(nullptr)}) {
            auto changed=std::string_view(field)=="state" ? state : binding;
            if (version.is_null()) changed.erase("version");
            else changed["version"]=version;
            {
                Database db(path);
                db.exec(std::string("UPDATE task_checkpoints SET ") + field + "='" + changed.dump() + "'");
            }
            const auto original=bytes(path);
            rejects([&] { (void)reader.task(task); },"unsupported checkpoint state or binding accepted");
            check(bytes(path)==original,"checkpoint format refusal rewrote stored records");
        }
        Database db(path);
        db.exec(std::string("UPDATE task_checkpoints SET ") + field + "='" +
                (std::string_view(field)=="state" ? state : binding).dump() + "'");
    }
    for (const auto* version : {"1","3","2.5"}) {
        {
            Database db(path);
            db.exec(std::string("UPDATE events SET schema_version=") + version + ",payload='not JSON';");
        }
        const auto event_bytes=bytes(path);
        bool version_rejected=false;
        try { (void)reader.events(); }
        catch (const std::runtime_error& error) {
            version_rejected=std::string(error.what())=="Unsupported event schema version";
        }
        check(version_rejected,"unsupported event version was parsed or returned");
        check(bytes(path)==event_bytes,"event refusal rewrote stored records");
    }
    {
        Database db(path);
        db.exec("PRAGMA user_version=3;");
    }
    const auto original=bytes(path);
    rejects([&] { (void)reader.tasks(); },"reader reused cached schema for task list");
    rejects([&] { (void)reader.events(); },"reader reused cached schema for events");
    rejects([&] { (void)reader.task(task); },"reader reused cached schema for task detail");
#ifdef _WIN32
    rejects([&] { (void)reader.build_proposal(task,action); },"reader reused cached schema for proposal");
#endif
    check(bytes(path)==original,"schema refusal modified existing state");
    Database db(path);
    check(db.value("PRAGMA journal_mode")=="delete" && db.value("PRAGMA user_version")=="3",
          "schema refusal migrated state or changed journal settings");
}
}

void run_state_reader_tests() {
    absent_state_and_validation();
    uninitialized_state_reopens();
    committed_pages();
    unsupported_schemas_are_rejected_without_changes();
    supervisor_committed_reads_and_bounds();
    summaries_and_size_bounds();
#ifdef _WIN32
    proposal_reads_are_bounded_and_committed();
#endif
    current_schema_and_checkpoint_versions_are_required();
}
