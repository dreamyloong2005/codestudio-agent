#include "codestudio/store.hpp"
#include "codestudio/state_reader.hpp"
#include <sqlite3.h>
#include <chrono>
#include <stdexcept>

namespace {
using namespace codestudio;
void check(bool ok,const char* message) { if (!ok) throw std::runtime_error(std::string("cron store: ")+message); }
template<class F> void rejects(F action,const char* message) {
    bool rejected=false; try { action(); } catch (const std::exception&) { rejected=true; }
    check(rejected,message);
}
std::int64_t minute() {
    const auto value=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return value-value%60000;
}
struct Temp {
    std::filesystem::path root=std::filesystem::temp_directory_path()/("codestudio-cron-store-"+new_id());
    Temp() { std::filesystem::create_directory(root); }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(root,ignored); }
};
struct Database {
    sqlite3* db=nullptr;
    explicit Database(const std::filesystem::path& path) {
        if (sqlite3_open(path_utf8(path).c_str(),&db)!=SQLITE_OK) {
            sqlite3_close(db); db=nullptr; throw std::runtime_error("cron test database open failed");
        }
    }
    ~Database() { sqlite3_close(db); }
    void exec(const std::string& command) {
        char* error=nullptr; const auto result=sqlite3_exec(db,command.c_str(),nullptr,nullptr,&error);
        const std::string message=error ? error : "cron SQL fixture failed"; sqlite3_free(error);
        if (result!=SQLITE_OK) throw std::runtime_error(message);
    }
    std::int64_t count(const char* text) {
        sqlite3_stmt* statement=nullptr;
        if (sqlite3_prepare_v2(db,text,-1,&statement,nullptr)!=SQLITE_OK) throw std::runtime_error("cron count prepare failed");
        const auto result=sqlite3_step(statement);
        const auto value=sqlite3_column_int64(statement,0); sqlite3_finalize(statement);
        if (result!=SQLITE_ROW) throw std::runtime_error("cron count query failed");
        return value;
    }
};
constexpr auto prompt="Perform the explicitly granted recurring engineering check";
Json binding(const std::string& workspace="fixture-workspace") {
    return {{"version",2},{"workspace",workspace},{"provider","fixture"},
        {"policy",Json::object()},{"tools",Json::array()},{"configuration",Json::object()}};
}
Json state() {
    return {{"version",2},{"phase","ready"},{"resumable",true},{"turns",0},{"max_turns",8},
        {"messages",Json::array({{{"role","system"},{"content","Fixture system instructions"}},
            {{"role","user"},{"content",prompt}}})},
        {"pending_calls",Json::array()},{"next_call",0},{"calls_seen",Json::array()},
        {"tool_failed",false},{"blocked_reason",""}};
}
Json argv(const std::string& workspace="fixture-workspace") {
    return Json::array({"codestudio-agent","resume","cron-task","--workspace",workspace,"--state","fixture.sqlite3"});
}
Json initial(const std::string& workspace="fixture-workspace") {
    return {{"version",2},{"state",state()},{"binding",binding(workspace)}};
}
Json create(Store& store,std::int64_t now,std::int64_t max_runs=0,const std::string& workspace="fixture-workspace") {
    return store.create_cron("Fixture recurring check","* * * * *","UTC",prompt,"fixture",argv(workspace),
        initial(workspace),86400000,8,100,max_runs,now);
}
std::string id(const Json& value) { return value.at("id").get<std::string>(); }
std::string task(const Json& job) { return job.at("task_id").get<std::string>(); }
void complete(Store& store,const Json& job,std::int64_t generation,std::int64_t now) {
    const auto claimed=store.claim_schedule(id(job),"owner",generation,now);
    check(!claimed.is_null(),"expected runnable occurrence");
    store.resume_task(task(job),binding(),id(job),claimed.at("claim_token").get<std::string>());
    auto done=state(); done["turns"]=1; done["phase"]="finished"; done["resumable"]=false;
    store.finish_task(task(job),"completed","Fixture complete",&done);
}

void registration_and_readers() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; StateReader reader(path);
    check(reader.crons()["items"].empty() && !std::filesystem::exists(path),"read-only query created state");
    rejects([&] { reader.cron("missing"); },"missing cron returned a record");
    Store store(path); const auto at=minute(); const auto first=create(store,at-1);
    check(first.at("next_due_at_ms")==at && first.at("run_count")==0 && first.at("status")=="active","registration consumed first occurrence");
    check(!first.contains("initial_state") && first==reader.cron(id(first)),"reader projection differs or leaks duplicate checkpoint");
    check(store.list_tasks().empty() && store.pending_schedules().empty(),"registration created runnable work");
    const auto second=create(store,at); const auto page=reader.crons(0,1);
    check(page.at("has_more")==true && page.at("items")[0].at("id")==second.at("id"),"cron page order failed");
    check(reader.crons(page.at("next_before").get<std::int64_t>(),1).at("items")[0].at("id")==first.at("id"),"cron page skipped record");
    rejects([&] { reader.crons(-1); },"negative cron cursor accepted");
    rejects([&] { store.crons(0,101); },"oversized cron page accepted");
    auto secret=argv(); secret.push_back("--api-key"); secret.push_back("not-persistent");
    rejects([&] { store.create_cron("bad","* * * * *","UTC",prompt,"fixture",secret,initial(),86400000,8,100,0,at); },"inline credential accepted");
    auto used=initial(); used["state"]["turns"]=1;
    rejects([&] { store.create_cron("bad","* * * * *","UTC",prompt,"fixture",argv(),used,86400000,8,100,0,at); },"used conversation became recurring template");
    rejects([&] { store.create_cron("bad","* * * * *","UTC","different prompt","fixture",argv(),initial(),86400000,8,100,0,at); },"prompt changed outside checkpoint");
    rejects([&] { create(store,at,-1); },"negative occurrence budget accepted");
    rejects([&] { store.create_cron("bad","* * * * *","UTC",prompt,"fixture",argv(),initial(),604800001,8,100,0,at); },"unbounded per-run deadline accepted");
    check(store.crons()["items"].size()==2,"rejected definition left a record");
    Database raw(path); check(raw.count("PRAGMA user_version")==7,"schema is not current");
}

void materialization_atomicity() {
    const std::vector<std::string> faults{
        "CREATE TRIGGER fault BEFORE INSERT ON tasks BEGIN SELECT RAISE(ABORT,'task fault'); END",
        "CREATE TRIGGER fault BEFORE INSERT ON task_checkpoints BEGIN SELECT RAISE(ABORT,'checkpoint fault'); END",
        "CREATE TRIGGER fault BEFORE INSERT ON scheduler_jobs BEGIN SELECT RAISE(ABORT,'job fault'); END",
        "CREATE TRIGGER fault BEFORE INSERT ON cron_occurrences BEGIN SELECT RAISE(ABORT,'slot fault'); END",
        "CREATE TRIGGER fault BEFORE UPDATE ON cron_schedules BEGIN SELECT RAISE(ABORT,'advance fault'); END",
        "CREATE TRIGGER fault BEFORE INSERT ON events BEGIN SELECT RAISE(ABORT,'event fault'); END"};
    for (const auto& fault : faults) {
        Temp temp; const auto path=temp.root/"state.sqlite3"; Store store(path); Database raw(path);
        const auto at=minute(); const auto definition=create(store,at-1); const auto generation=store.start_scheduler("owner",1,at);
        raw.exec(fault);
        rejects([&] { store.materialize_crons("fixture-workspace","owner",generation,at); },"materialization fault ignored");
        check(store.cron(id(definition))==definition && store.list_tasks().empty() && store.schedules()["items"].empty(),"fault partially advanced definition or created work");
        check(raw.count("SELECT count(*) FROM task_checkpoints")==0 && raw.count("SELECT count(*) FROM cron_occurrences")==0 &&
            raw.count("SELECT count(*) FROM events")==0,"fault committed checkpoint, slot, or audit");
        raw.exec("DROP TRIGGER fault");
        const auto jobs=store.materialize_crons("fixture-workspace","owner",generation,at);
        check(jobs.size()==1 && raw.count("SELECT count(*) FROM cron_occurrences")==1,"retry did not create exactly one slot");
        check(store.task(task(jobs[0])).at("status")=="queued" && store.checkpoint_revision(task(jobs[0]))==1,"new task/checkpoint was not queued atomically");
        check(store.checkpoint(task(jobs[0]))==initial() && jobs[0].at("resume_argv")[2]==jobs[0].at("task_id"),"new occurrence lost its frozen initial state");
        check(store.materialize_crons("fixture-workspace","owner",generation,at).empty(),"same tick duplicated an occurrence");
    }
}

void coalescing_nonoverlap_budgets_and_fencing() {
    Temp temp; Store store(temp.root/"state.sqlite3"); const auto at=minute();
    const auto definition=create(store,at-1,2); const auto generation=store.start_scheduler("owner",1,at);
    rejects([&] { store.materialize_crons("fixture-workspace","stale",generation,at); },"stale owner materialized work");
    check(store.materialize_crons("other-workspace","owner",generation,at).empty(),"worker materialized another workspace");
    check(store.materialize_crons("fixture-workspace","owner",generation,at-1).empty(),"cron dispatched early");
    const auto late=at+10*60000+1234;
    const auto first=store.materialize_crons("fixture-workspace","owner",generation,late);
    check(first.size()==1 && store.cron(id(definition)).at("last_due_at_ms")==at+10*60000,"missed slots did not coalesce to latest UTC slot");
    check(first[0].at("deadline_at_ms")==late+86400000,"catch-up used an already-expired original deadline");
    check(store.materialize_crons("fixture-workspace","owner",generation,late+60000).empty(),"active occurrence overlapped");
    check(store.cron(id(definition)).at("run_count")==1,"overlap consumed an occurrence budget");
    complete(store,first[0],generation,late);
    check(store.materialize_crons("fixture-workspace","owner",generation,late).empty(),"clock rollback repeated consumed slot");
    const auto second=store.materialize_crons("fixture-workspace","owner",generation,at+12*60000);
    check(second.size()==1 && task(second[0])!=task(first[0]) && second[0].at("budget_turns")==8,"next occurrence did not get independent task/budget");
    check(store.cron(id(definition)).at("status")=="exhausted" && store.cron(id(definition)).at("run_count")==2,"max_runs did not exhaust series");
    complete(store,second[0],generation,at+12*60000);
    check(store.materialize_crons("fixture-workspace","owner",generation,at+13*60000).empty(),"exhausted series created more tasks");
    rejects([&] { store.control_cron(id(definition),"resume",late); },"exhausted series silently gained budget");
    store.stop_scheduler();
    rejects([&] { store.materialize_crons("fixture-workspace","owner",generation,late); },"stopped worker materialized work");
}

void controls_and_manual_takeover() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; Store store(path); Database raw(path);
    const auto at=minute(); const auto definition=create(store,at-1); const auto generation=store.start_scheduler("owner",1,at);
    auto jobs=store.materialize_crons("fixture-workspace","owner",generation,at); const auto first=jobs[0];
    store.control_cron(id(definition),"pause",at);
    check(store.schedule(id(first)).at("status")=="queued","pause cancelled its existing occurrence");
    check(store.materialize_crons("fixture-workspace","owner",generation,at+2*60000).empty(),"paused series dispatched");
    const auto resumed=store.control_cron(id(definition),"resume",at+2*60000);
    check(resumed.at("next_due_at_ms")==at+3*60000,"resume backfilled paused slots");
    store.resume_task(task(first),binding());
    check(store.schedule(id(first)).at("status")=="cancelled","manual takeover retained scheduled authorization");
    check(store.materialize_crons("fixture-workspace","owner",generation,at+3*60000).empty(),"cron overlapped manual takeover");
    raw.exec("UPDATE tasks SET status='waiting_approval' WHERE id='"+task(first)+"'");
    check(store.materialize_crons("fixture-workspace","owner",generation,at+4*60000).empty(),"cron overlapped a manual task awaiting approval");
    raw.exec("UPDATE tasks SET status='running' WHERE id='"+task(first)+"'");
    auto done=state(); done["phase"]="finished"; done["resumable"]=false;
    store.finish_task(task(first),"completed","manual complete",&done);
    jobs=store.materialize_crons("fixture-workspace","owner",generation,at+5*60000); const auto second=jobs[0];
    const auto claim=store.claim_schedule(id(second),"owner",generation,at+5*60000);
    store.resume_task(task(second),binding(),id(second),claim.at("claim_token").get<std::string>());
    raw.exec("CREATE TRIGGER cancel_fault BEFORE UPDATE ON cron_schedules WHEN NEW.status='cancelled' BEGIN SELECT RAISE(ABORT,'cancel fault'); END");
    rejects([&] { store.control_cron(id(definition),"cancel",at+4*60000); },"cancel failure ignored");
    check(store.cron(id(definition)).at("status")=="active" && store.schedule(id(second)).at("cancel_requested")==false,"series cancellation did not roll back job cancellation");
    raw.exec("DROP TRIGGER cancel_fault"); store.control_cron(id(definition),"cancel",at+4*60000);
    check(store.schedule(id(second)).at("cancel_requested")==true && store.schedule(id(second)).at("status")=="running","series cancellation lost active association");
    store.finish_task(task(second),"cancelled","observed");
    check(store.materialize_crons("fixture-workspace","owner",generation,at+5*60000).empty(),"cancelled series dispatched");
    rejects([&] { store.control_cron(id(definition),"resume",at); },"cancelled series resurrected");
    check(store.control_cron(id(definition),"cancel",at).at("status")=="cancelled","cancel is not idempotent");
    const auto another=create(store,at-1); jobs=store.materialize_crons("fixture-workspace","owner",generation,at);
    const auto pending=jobs[0]; const auto claimed=store.claim_schedule(id(pending),"owner",generation,at);
    store.control_cron(id(another),"cancel",at);
    check(store.schedule(id(pending)).at("status")=="cancelled","cancel left a claimed occurrence runnable");
    rejects([&] { store.resume_task(task(pending),binding(),id(pending),claimed.at("claim_token").get<std::string>()); },"cancelled series claim started");
}

void unknown_effects_pause_series() {
    Temp temp; Store store(temp.root/"state.sqlite3"); const auto at=minute(); const auto definition=create(store,at-1);
    const auto generation=store.start_scheduler("owner",1,at);
    const auto job=store.materialize_crons("fixture-workspace","owner",generation,at)[0];
    const auto claim=store.claim_schedule(id(job),"owner",generation,at);
    store.resume_task(task(job),binding(),id(job),claim.at("claim_token").get<std::string>());
    const auto action=store.prepare_action(task(job),{"effect","process",Json::object()}); store.dispatch_action(action);
    const auto replacement=store.start_scheduler("owner",2,at+1); store.recover_schedules("owner",replacement,at+1);
    check(store.schedule(id(job)).at("status")=="blocked","unknown dispatched action did not block occurrence");
    check(store.materialize_crons("fixture-workspace","owner",replacement,at+1).empty(),"uncertain outcome created new occurrence");
    check(store.cron(id(definition)).at("status")=="paused" && !store.cron(id(definition)).at("last_error").get_ref<const std::string&>().empty(),"series did not pause before next due time");
    rejects([&] { store.control_cron(id(definition),"resume",at+2); },"unknown effect bypassed by recurring authorization");
    check(store.materialize_crons("fixture-workspace","owner",replacement,at+60000).empty() && store.list_tasks().size()==1,"unknown effect replayed as new task");
}

void completed_occurrences_do_not_backfill_overlapping_slots() {
    for (const bool manual : {false,true}) {
        Temp temp; Store store(temp.root/"state.sqlite3"); const auto at=minute()-120000;
        const auto definition=create(store,at-1); const auto generation=store.start_scheduler("owner",1,at);
        const auto first=store.materialize_crons("fixture-workspace","owner",generation,at)[0];
        if (manual) {
            store.resume_task(task(first),binding());
            auto done=state(); done["phase"]="finished"; done["resumable"]=false;
            store.finish_task(task(first),"completed","manual complete",&done);
        } else complete(store,first,generation,at);
        const auto ended=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        check(store.materialize_crons("fixture-workspace","owner",generation,ended).empty(),
            "finished occurrence backfilled a slot from its own active lifetime");
        check(store.cron(id(definition)).at("run_count")==1,"overlapping slot consumed recurring budget after completion");
        const auto idle_restart=ended-ended%60000+5*60000;
        const auto next=store.materialize_crons("fixture-workspace","owner",generation,idle_restart);
        check(next.size()==1 && store.cron(id(definition)).at("last_due_at_ms")==idle_restart,
            "nonoverlap fix suppressed legitimate idle catch-up");
    }
}

void completion_and_skip_commit_together() {
    for (const bool manual : {false,true}) {
        Temp temp; const auto path=temp.root/"state.sqlite3"; Store store(path); Database raw(path);
        const auto at=minute()-120000; const auto definition=create(store,at-1);
        const auto generation=store.start_scheduler("owner",1,at);
        const auto job=store.materialize_crons("fixture-workspace","owner",generation,at)[0];
        if (manual) store.resume_task(task(job),binding());
        else {
            const auto claim=store.claim_schedule(id(job),"owner",generation,at);
            store.resume_task(task(job),binding(),id(job),claim.at("claim_token").get<std::string>());
        }
        // Manual takeover already advances the schedule when cancelling its
        // old job; rewind only this cursor to model a later task completion.
        raw.exec("UPDATE cron_schedules SET next_due_at_ms="+std::to_string(at+60000)+" WHERE id='"+id(definition)+"'");
        const auto before=store.cron(id(definition)); const auto saved=store.checkpoint(task(job));
        const auto revision=store.checkpoint_revision(task(job)); const auto prior_job=store.schedule(id(job));
        const auto events=raw.count("SELECT count(*) FROM events");
        raw.exec("CREATE TRIGGER completion_fault BEFORE UPDATE ON cron_schedules BEGIN SELECT RAISE(ABORT,'completion fault'); END");
        auto done=state(); done["phase"]="finished"; done["resumable"]=false;
        rejects([&] { store.finish_task(task(job),"completed","finished",&done); },"cron completion fault was ignored");
        check(store.task(task(job)).at("status")=="running" && store.schedule(id(job))==prior_job &&
            store.checkpoint(task(job))==saved && store.checkpoint_revision(task(job))==revision &&
            store.cron(id(definition))==before && raw.count("SELECT count(*) FROM events")==events,
            "cron cursor fault partially committed the task, job, checkpoint, or audit");
        raw.exec("DROP TRIGGER completion_fault"); store.finish_task(task(job),"completed","finished",&done);
        check(store.cron(id(definition)).at("next_due_at_ms").get<std::int64_t>()>minute(),
            "successful completion did not persist skipped active slots");
    }
}

void capacity_does_not_consume_due_slots() {
    Temp temp; Store store(temp.root/"state.sqlite3"); const auto at=minute(); const auto definition=create(store,at-1);
    const auto generation=store.start_scheduler("owner",1,at); std::string release_job;
    for (int index=0;index<100;++index) {
        const auto task_id=store.create_task("manual queue","fixture-workspace","fixture");
        store.save_checkpoint(task_id,state(),binding()); store.finish_task(task_id,"cancelled","paused");
        auto arguments=argv(); arguments[2]=task_id;
        const auto job=store.schedule_resume(task_id,arguments,binding(),1,at,at+86400000,8,100);
        if (index==0) release_job=id(job);
    }
    check(store.materialize_crons("fixture-workspace","owner",generation,at).empty() && store.cron(id(definition))==definition,"full queue consumed due slot or budget");
    store.cancel_schedule(release_job,at);
    check(store.materialize_crons("fixture-workspace","owner",generation,at).size()==1,"released queue capacity not used");
    for (int index=1;index<100;++index) create(store,at);
    rejects([&] { create(store,at); },"unbounded recurring definitions accepted");
    store.control_cron(id(definition),"cancel",at);
    check(create(store,at).at("status")=="active","cancel did not release definition capacity");
}
}

void run_cron_store_tests() {
    registration_and_readers(); materialization_atomicity(); coalescing_nonoverlap_budgets_and_fencing();
    controls_and_manual_takeover(); unknown_effects_pause_series(); capacity_does_not_consume_due_slots();
    completed_occurrences_do_not_backfill_overlapping_slots();
    completion_and_skip_commit_together();
}
