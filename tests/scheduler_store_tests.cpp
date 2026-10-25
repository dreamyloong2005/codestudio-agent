#include "codestudio/store.hpp"
#include "codestudio/state_reader.hpp"
#include "codestudio/digest.hpp"
#include <sqlite3.h>
#include <chrono>
#include <stdexcept>

namespace {
using namespace codestudio;
void check(bool value,const char* message) { if (!value) throw std::runtime_error(std::string("scheduler: ")+message); }
template<class F> void rejects(F action,const char* message) {
    bool caught=false; try { action(); } catch (const std::exception&) { caught=true; }
    check(caught,message);
}
std::int64_t now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
struct Temp {
    std::filesystem::path root=std::filesystem::temp_directory_path()/("codestudio-scheduler-"+new_id());
    Temp() { std::filesystem::create_directory(root); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root,error); }
};
struct Database {
    sqlite3* db=nullptr;
    explicit Database(const std::filesystem::path& path) {
        if (sqlite3_open(path_utf8(path).c_str(),&db)!=SQLITE_OK) {
            sqlite3_close(db); db=nullptr; throw std::runtime_error("scheduler test open failed");
        }
    }
    ~Database() { sqlite3_close(db); }
    void exec(const std::string& command) {
        char* error=nullptr; const auto code=sqlite3_exec(db,command.c_str(),nullptr,nullptr,&error);
        const std::string message=error ? error : "scheduler test SQL failed"; sqlite3_free(error);
        if (code!=SQLITE_OK) throw std::runtime_error(message);
    }
};
Json state(const std::string& phase="model_pending") {
    return {{"version",2},{"phase",phase},{"resumable",true},{"turns",1},{"max_turns",8},
        {"messages",Json::array()},{"pending_calls",Json::array()},{"next_call",0},
        {"calls_seen",Json::array()},{"tool_failed",false},{"blocked_reason",""}};
}
Json binding() { return {{"version",2},{"provider","fixture"},{"workspace","fixture-workspace"}}; }
std::string paused(Store& store,const std::string& phase="model_pending") {
    const auto task=store.create_task("resume fixture","fixture-workspace","fixture");
    store.save_checkpoint(task,state(phase),binding()); store.finish_task(task,"cancelled","fixture paused"); return task;
}
Json enroll(Store& store,const std::string& task,int attempts=8,std::int64_t delay=100) {
    return store.schedule_resume(task,Json::array({"codestudio-agent","resume",task}),binding(),
        store.checkpoint_revision(task),now(),now()+86400000,attempts,delay);
}
std::string id(const Json& job) { return job.at("id").get<std::string>(); }
std::string token(const Json& job) { return job.at("claim_token").get<std::string>(); }

SupervisorRunOptions supervisor_options(const std::string& id="run-one", const std::string& owner="supervisor-one") {
    return {id,owner,"fixture-workspace","fixture-process-identity",123,100,2,1000};
}
void supervisor_lifecycle_and_events() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; Store store(path); StateReader reader(path);
    const auto options=supervisor_options();
    check(store.supervisor().is_null() && reader.supervisor().is_null(),"new database invented supervisor history");
    store.start_supervisor(options,{0,0},1000);
    auto initial=store.supervisor();
    check(initial==reader.supervisor() && initial["phase"]=="starting" && initial["last_exit_code"].is_null(),"supervisor registration lost initial nullable exit");
    check(reader.scheduler_snapshot()["worker"]==store.scheduler() &&
        reader.scheduler_snapshot()["supervisor"]==initial,"coherent scheduler snapshot differs");
    SupervisorRunState current{"starting",0,234,0};
    check(store.update_supervisor(options.id,options.owner,current,1001,"supervisor.starting"),"child spawn was not recorded");
    const auto generation=store.start_scheduler(options.owner,234,1002,SchedulerStartFence{0,0});
    current.phase="running"; current.generation=generation;
    check(store.update_supervisor(options.id,options.owner,current,1003,"supervisor.running"),"child registration was not recorded");
    const auto count=reader.supervisor_events()["items"].size();
    check(store.heartbeat_supervisor(options.id,options.owner,1004),"current supervisor heartbeat rejected");
    check(reader.supervisor_events()["items"].size()==count && reader.supervisor()["heartbeat_at_ms"]==1004,
        "heartbeat flooded supervisor events or did not update its time");
    current.worker_pid=0; current.last_exit_code=7;
    check(store.update_supervisor(options.id,options.owner,current,1005,"supervisor.child_exited"),"known child exit rejected");
    current.phase="backoff"; current.next_restart_at_ms=2006;
    check(store.update_supervisor(options.id,options.owner,current,1006,"supervisor.backoff"),"backoff was not recorded");
    check(reader.supervisor()["last_exit_code"]==7 && reader.supervisor()["next_restart_at_ms"]==2006,"backoff diagnostics incomplete");
    current.phase="starting"; current.restarts=1; current.worker_pid=345; current.next_restart_at_ms=0;
    check(store.update_supervisor(options.id,options.owner,current,2006,"supervisor.starting"),"replacement launch rejected");
    store.start_scheduler(options.owner,345,2007,SchedulerStartFence{0,generation});
    current.phase="running"; ++current.generation;
    check(store.update_supervisor(options.id,options.owner,current,2008,"supervisor.running"),"replacement registration rejected");
    current.worker_pid=0; current.last_exit_code=0;
    check(store.update_supervisor(options.id,options.owner,current,2009,"supervisor.child_exited"),"successful exit rejected");
    current.phase="stopped"; current.exit_reason="normal_exit";
    check(store.update_supervisor(options.id,options.owner,current,2010,"supervisor.stopped"),"terminal state rejected");
    const auto finished=store.supervisor();
    check(finished["restarts"]==1 && finished["finished_at_ms"]==2010 && finished["last_exit_code"]==0,
        "terminal state lost cumulative budget or exit evidence");
    check(!store.heartbeat_supervisor(options.id,options.owner,2011) &&
        !store.update_supervisor(options.id,options.owner,current,2011,"supervisor.stopped"),"terminal lifecycle accepted mutation");
    check(reader.events()["items"].empty(),"supervisor events leaked into task event stream");
    std::int64_t cursor=0; std::size_t seen=0;
    for (;;) {
        const auto page=reader.supervisor_events(cursor,2);
        for (const auto& event : page["items"]) {
            check(event["run_id"]==options.id && event["payload"]["id"]==options.id && event["seq"].get<std::int64_t>()>cursor,
                "supervisor event pagination crossed identity or cursor");
            ++seen;
        }
        cursor=page["next_after"].get<std::int64_t>();
        if (!page["has_more"].get<bool>()) break;
    }
    check(seen==9 && reader.supervisor_events(cursor)["items"].empty() &&
        reader.supervisor_events(cursor)["next_after"]==cursor,"supervisor pagination lost events or reset empty tail");
}
void supervisor_atomicity_and_ownership() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; Store store(path); StateReader reader(path); Database raw(path);
    const auto first=supervisor_options();
    raw.exec("CREATE TRIGGER supervisor_start_fault BEFORE INSERT ON supervisor_events BEGIN SELECT RAISE(ABORT,'start fault'); END");
    rejects([&] { store.start_supervisor(first,{0,0},1000); },"supervisor event failure ignored at registration");
    check(store.supervisor().is_null() && reader.supervisor_events()["items"].empty(),"supervisor registration was not atomic");
    raw.exec("DROP TRIGGER supervisor_start_fault"); store.start_supervisor(first,{0,0},1000);
    const auto original=store.supervisor();
    raw.exec("CREATE TRIGGER supervisor_update_fault BEFORE INSERT ON supervisor_events WHEN NEW.kind='supervisor.stopping' BEGIN SELECT RAISE(ABORT,'update fault'); END");
    SupervisorRunState stopping{"stopping",0,0,0,0,std::nullopt,"cancelled"};
    rejects([&] { store.update_supervisor(first.id,first.owner,stopping,1001,"supervisor.stopping"); },"supervisor event failure ignored at transition");
    check(store.supervisor()==original && reader.supervisor_events()["items"].size()==1,"supervisor update and event were not atomic");
    raw.exec("DROP TRIGGER supervisor_update_fault");
    check(!store.update_supervisor(first.id,"wrong-owner",stopping,1001,"supervisor.stopping") &&
        !store.heartbeat_supervisor(first.id,"wrong-owner",1001),"foreign owner wrote lifecycle");
    const auto second=supervisor_options("run-two","supervisor-two");
    rejects([&] { store.start_supervisor(second,{1,0},1002); },"stale stop epoch admitted supervisor");
    rejects([&] { store.start_supervisor(second,{0,1},1002); },"stale generation admitted supervisor");
    raw.exec("CREATE TRIGGER supervisor_replacement_fault BEFORE INSERT ON supervisor_events WHEN NEW.run_id='run-two' BEGIN SELECT RAISE(ABORT,'replacement fault'); END");
    rejects([&] { store.start_supervisor(second,{0,0},1002); },"replacement event fault ignored");
    check(store.supervisor()==original && reader.supervisor_events()["items"].size()==1,
        "failed replacement partially superseded prior lifecycle");
    raw.exec("DROP TRIGGER supervisor_replacement_fault");
    rejects([&] { store.start_supervisor(first,{0,0},1002); },"duplicate lifecycle id reused");
    check(store.supervisor()==original,"duplicate lifecycle id rewrote prior history");
    store.start_supervisor(second,{0,0},1002);
    const auto events=reader.supervisor_events()["items"];
    check(events.size()==3 && events[1]["run_id"]==first.id && events[1]["kind"]=="supervisor.stopped" &&
        events[1]["payload"]["exit_reason"]=="superseded" && events[1]["payload"]["last_exit_code"].is_null(),
        "replacement lost prior lifecycle or invented observed child exit");
    check(!store.update_supervisor(first.id,first.owner,stopping,1003,"supervisor.stopping") &&
        !store.heartbeat_supervisor(first.id,first.owner,1003),"superseded owner mutated history");
    check(store.supervisor()["id"]==second.id,"stale writer replaced current supervisor");
    store.stop_scheduler();
    rejects([&] { store.start_supervisor(supervisor_options("run-three","supervisor-three"),{0,0},1004); },"stop fence race admitted supervisor");
    check(store.supervisor()["id"]==second.id && reader.supervisor_events()["items"].size()==3,"fence failure wrote lifecycle events");
}
void supervisor_input_boundaries() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; Store store(path); StateReader reader(path);
    auto options=supervisor_options();
    for (const auto invalid : {-1,101}) {
        options.max_restarts=invalid; rejects([&] { store.start_supervisor(options,{0,0},1000); },"invalid restart cap admitted");
    }
    options=supervisor_options(); options.poll_ms=19;
    rejects([&] { store.start_supervisor(options,{0,0},1000); },"short polling admitted");
    options.poll_ms=5001; rejects([&] { store.start_supervisor(options,{0,0},1000); },"long polling admitted");
    options=supervisor_options(); options.restart_delay_ms=99;
    rejects([&] { store.start_supervisor(options,{0,0},1000); },"short backoff admitted");
    options.restart_delay_ms=60001; rejects([&] { store.start_supervisor(options,{0,0},1000); },"long backoff admitted");
    options=supervisor_options(); options.process_identity="";
    rejects([&] { store.start_supervisor(options,{0,0},1000); },"empty supervisor process identity admitted");
    options=supervisor_options(); options.workspace=std::string("bad\0path",8);
    rejects([&] { store.start_supervisor(options,{0,0},1000); },"NUL supervisor workspace admitted");
    options=supervisor_options();
    rejects([&] { store.start_supervisor(options,{-1,0},1000); },"negative supervisor fence admitted");
    rejects([&] { store.start_supervisor(options,{0,0},0); },"zero supervisor timestamp admitted");
    store.start_supervisor(options,{0,0},1000);
    const auto initial=store.supervisor();
    auto invalid=[&](SupervisorRunState value,const char* kind="supervisor.starting") {
        rejects([&] { store.update_supervisor(options.id,options.owner,value,1001,kind); },"invalid supervisor state accepted");
        check(store.supervisor()==initial && reader.supervisor_events()["items"].size()==1,"invalid supervisor state changed records");
    };
    invalid({"unknown"}); invalid({"starting",1}); invalid({"starting",0,-1}); invalid({"starting",0,0,-1});
    invalid({"starting",0,0,2});
    invalid({"starting",0,0,0,1002}); invalid({"starting",0,0,0,0,7});
    invalid({"backoff",0,0,0,2000,7},"supervisor.backoff");
    invalid({"running",0,0,1},"supervisor.running");
    invalid({"stopped",0,0,0},"supervisor.stopped");
    invalid({"stopped",0,1,0,0,std::nullopt,"cancelled"},"supervisor.stopped");
    invalid({"starting"},"supervisor.child_exited");
    check(store.heartbeat_supervisor(options.id,options.owner,999) && store.supervisor()["heartbeat_at_ms"]==1000,
        "clock correction lost ownership or regressed heartbeat");
    check(store.update_supervisor(options.id,options.owner,{"starting",0,234},998,"supervisor.starting") &&
        store.supervisor()["updated_at_ms"]==1000,"clock correction broke transition or regressed record time");
    store.start_supervisor(supervisor_options("after-clock-correction","new-owner"),{0,0},997);
    const auto clock_events=reader.supervisor_events()["items"];
    check(clock_events[clock_events.size()-2]["created_at_ms"]==1000 &&
        clock_events.back()["created_at_ms"]==997 && store.supervisor()["started_at_ms"]==997,
        "clock correction forged a new lifecycle time or regressed the superseded lifecycle");
    rejects([&] { reader.supervisor_events(-1); },"negative supervisor cursor accepted");
    rejects([&] { reader.supervisor_events(0,0); },"empty supervisor page accepted");
    rejects([&] { reader.supervisor_events(0,101); },"oversized supervisor page accepted");
}

void enrollment_and_readers() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; StateReader reader(path);
    check(reader.schedules()["items"].empty() && reader.scheduler()["state"]=="stopped","missing state was not empty");
    check(!std::filesystem::exists(path),"reader created state database");
    Store store(path); const auto task=paused(store);
    check(store.checkpoint_revision(task)==1,"initial revision was not one");
    store.resume_task(task,binding()); store.save_checkpoint(task,state(),binding());
    check(store.checkpoint_revision(task)==2,"checkpoint revision did not increment");
    store.finish_task(task,"cancelled","pause again"); const auto job=enroll(store,task);
    check(job==reader.schedule(id(job)) && reader.schedules()["items"].size()==1,"read-only job view differs");
    check(job.at("expected_revision")==2 && job.at("budget_turns")==8 && job.at("attempts")==0,"enrollment lost original grant");
    check(store.checkpoint(task).size()==3 && !store.checkpoint(task).contains("revision"),"checkpoint envelope changed");
    rejects([&] { enroll(store,task); },"duplicate active task job accepted");
    const auto second=paused(store); const auto argv=Json::array({"codestudio-agent","resume",second});
    rejects([&] { store.schedule_resume(second,argv,binding(),999,now(),0); },"stale revision enrolled");
    auto changed=binding(); changed["provider"]="other";
    rejects([&] { store.schedule_resume(second,argv,changed,1,now(),0); },"changed binding enrolled");
    auto secret=argv; secret.push_back("--api-key"); secret.push_back("must-not-save");
    rejects([&] { store.schedule_resume(second,secret,binding(),1,now(),0); },"inline credential enrolled");
    rejects([&] { store.schedule_resume(second,argv,binding(),1,now(),now()+8LL*86400000); },"unbounded deadline accepted");
    rejects([&] { store.schedule_resume(second,argv,binding(),1,now(),0,101); },"unbounded attempt count accepted");
    rejects([&] { store.schedule_resume(second,argv,binding(),1,now(),0,8,99); },"invalid retry delay accepted");
    const auto next=enroll(store,second); const auto page=reader.schedules(0,1);
    check(page["items"][0]["id"]==next["id"] && page["has_more"]==true,"job pagination order failed");
    check(reader.schedules(page["next_before"].get<std::int64_t>(),1)["items"][0]["id"]==job["id"],"job pagination lost row");
    store.cancel_schedule(id(job),now()); store.cancel_schedule(id(next),now());
    check(store.pending_schedules().empty(),"terminal jobs leaked into worker candidates");
}

void atomic_claim_resume_finish() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; Store store(path); Database raw(path);
    const auto task=paused(store); const auto job=enroll(store,task); const auto generation=store.start_scheduler("owner",1,now());
    raw.exec("CREATE TRIGGER claim_fault BEFORE INSERT ON events WHEN NEW.kind='scheduler.claimed' BEGIN SELECT RAISE(ABORT,'claim fault'); END;");
    rejects([&] { store.claim_schedule(id(job),"owner",generation,now()); },"claim fault ignored");
    check(store.schedule(id(job))["status"]=="queued" && store.schedule(id(job))["attempts"]==0,"failed claim consumed attempt");
    raw.exec("DROP TRIGGER claim_fault"); const auto claim=store.claim_schedule(id(job),"owner",generation,now());
    raw.exec("CREATE TRIGGER start_fault BEFORE INSERT ON events WHEN NEW.kind='scheduler.running' BEGIN SELECT RAISE(ABORT,'start fault'); END;");
    rejects([&] { store.resume_task(task,binding(),id(job),token(claim)); },"resume fault ignored");
    check(store.task(task)["status"]=="cancelled" && store.schedule(id(job))["status"]=="claimed","resume was not atomic");
    raw.exec("DROP TRIGGER start_fault"); store.resume_task(task,binding(),id(job),token(claim));
    const auto before=store.checkpoint_revision(task); auto done=state("finished"); done["resumable"]=false;
    raw.exec("CREATE TRIGGER finish_fault BEFORE INSERT ON events WHEN NEW.kind='scheduler.completed' BEGIN SELECT RAISE(ABORT,'finish fault'); END;");
    rejects([&] { store.finish_task(task,"completed","done",&done); },"finish fault ignored");
    check(store.task(task)["status"]=="running" && store.schedule(id(job))["status"]=="running" &&
        store.checkpoint_revision(task)==before && store.schedule(id(job))["expected_revision"]==before,"task/job/checkpoint finish was not atomic");
    raw.exec("DROP TRIGGER finish_fault"); store.finish_task(task,"completed","done",&done);
    check(store.schedule(id(job))["status"]=="completed" && store.schedule(id(job))["expected_revision"]==before+1,"completed job lost checkpoint revision");
    store.finish_scheduler_claim(id(job),token(claim),"late callback",now());
    check(store.schedule(id(job))["status"]=="completed","late claim completion rewrote final job");
}

void retry_budgets_and_deadlines() {
    Temp temp; Store store(temp.root/"state.sqlite3"); const auto generation=store.start_scheduler("owner",1,now());
    const auto task=paused(store); const auto job=enroll(store,task);
    auto claim=store.claim_schedule(id(job),"owner",generation,now()); store.resume_task(task,binding(),id(job),token(claim));
    auto failed=state(); failed["turns"]=2; store.finish_task(task,"failed","provider failed",&failed);
    auto queued=store.schedule(id(job)); check(queued["status"]=="queued" && queued["attempts"]==1,"known model failure was not queued");
    const auto due=queued["due_at_ms"].get<std::int64_t>();
    check(store.claim_schedule(id(job),"owner",generation,due-1).is_null() && store.schedule(id(job))["attempts"]==1,"early polling consumed attempt");
    claim=store.claim_schedule(id(job),"owner",generation,due); store.resume_task(task,binding(),id(job),token(claim));
    failed["turns"]=3; store.finish_task(task,"failed","again",&failed); queued=store.schedule(id(job));
    check(queued["due_at_ms"].get<std::int64_t>()-queued["updated_at_ms"].get<std::int64_t>()==200,"exponential backoff changed");
    claim=store.claim_schedule(id(job),"owner",generation,queued["due_at_ms"].get<std::int64_t>());
    store.resume_task(task,binding(),id(job),token(claim)); auto nonmodel=state("ready");
    store.finish_task(task,"failed","tool or validation failed",&nonmodel);
    check(store.schedule(id(job))["status"]=="blocked","nonmodel failure retried automatically");
    const auto limited=paused(store); const auto limited_job=enroll(store,limited,1);
    claim=store.claim_schedule(id(limited_job),"owner",generation,now()); store.resume_task(limited,binding(),id(limited_job),token(claim));
    store.finish_task(limited,"failed","failure",&failed);
    check(store.schedule(id(limited_job))["status"]=="exhausted","attempt cap ignored");
    const auto spent=paused(store); const auto spent_job=enroll(store,spent);
    claim=store.claim_schedule(id(spent_job),"owner",generation,now()); store.resume_task(spent,binding(),id(spent_job),token(claim));
    failed["turns"]=8; store.finish_task(spent,"failed","budget spent",&failed);
    check(store.schedule(id(spent_job))["status"]=="exhausted","original turn budget was extended");
    const auto late=paused(store); const auto late_job=enroll(store,late);
    check(store.claim_schedule(id(late_job),"owner",generation,late_job["deadline_at_ms"].get<std::int64_t>()).is_null(),"expired job claimed");
    check(store.schedule(id(late_job))["status"]=="expired" && store.schedule(id(late_job))["attempts"]==0,"expiration consumed attempt");
}

void fencing_cancellation_and_manual_resume() {
    Temp temp; Store store(temp.root/"state.sqlite3"); const auto task=paused(store); const auto job=enroll(store,task);
    check(store.cancel_schedule(id(job),now(),true)==job,"active-only cancellation changed queued job");
    const auto first=store.start_scheduler("first",1,now()); const auto old=store.claim_schedule(id(job),"first",first,now());
    const auto stopped=store.stop_scheduler(); check(stopped["stop_epoch"]==1 && stopped["owner"]=="first","stop lost audit registration");
    rejects([&] { store.resume_task(task,binding(),id(job),token(old)); },"stopped owner started task");
    const auto second=store.start_scheduler("second",2,now()); check(second>first,"generation did not advance");
    check(!store.heartbeat_scheduler("first",now()),"old owner heartbeat accepted");
    store.finish_scheduler("first"); check(store.scheduler()["state"]=="running","old owner stopped replacement");
    store.recover_schedules("second",second,now()); const auto current=store.claim_schedule(id(job),"second",second,now());
    rejects([&] { store.resume_task(task,binding(),id(job),token(old)); },"old claim token resumed replacement");
    store.finish_scheduler_claim(id(job),token(old),"stale",now()); check(store.schedule(id(job))["status"]=="claimed","stale completion rewrote claim");
    store.resume_task(task,binding(),id(job),token(current)); const auto cancelled=store.cancel_schedule(id(job),now(),true);
    check(cancelled["status"]=="running" && cancelled["cancel_requested"]==true,"active cancellation discarded running association");
    store.finish_task(task,"cancelled","cancellation observed"); check(store.schedule(id(job))["status"]=="cancelled","active cancellation did not finish");
    const auto manual=paused(store); const auto manual_job=enroll(store,manual); store.resume_task(manual,binding());
    check(store.schedule(id(manual_job))["status"]=="cancelled" && store.task(manual)["status"]=="running","manual resume left queued authorization");
    const auto not_started=paused(store); const auto claimed_job=enroll(store,not_started);
    const auto claimed=store.claim_schedule(id(claimed_job),"second",second,now());
    check(store.cancel_schedule(id(claimed_job),now(),true)["status"]=="cancelled","active-only cancellation missed claimed job");
    rejects([&] { store.resume_task(not_started,binding(),id(claimed_job),token(claimed)); },"cancelled claim started task");
}

void recovery_is_scoped_and_preserves_receipts() {
    for (const auto* mode : {"prepared","dispatched","succeeded"}) {
        Temp temp; Store store(temp.root/"state.sqlite3");
        const auto foreign=store.create_task("foreground","fixture-workspace","fixture");
        const auto foreign_action=store.prepare_action(foreign,{"foreign","write",Json::object()}); store.dispatch_action(foreign_action);
        const auto task=paused(store,"tools"); const auto job=enroll(store,task); const auto generation=store.start_scheduler("old",1,now());
        const auto claim=store.claim_schedule(id(job),"old",generation,now()); store.resume_task(task,binding(),id(job),token(claim));
        const auto action=store.prepare_action(task,{"work","write",Json::object()}); const std::string selected=mode;
        if (selected!="prepared") store.dispatch_action(action);
        if (selected=="succeeded") store.finish_action(action,{true,{{"marker","durable receipt"}},"","applied"});
        const auto next=store.start_scheduler("new",2,now()); const auto recovered=store.recover_schedules("new",next,now());
        check(recovered.size()==1 && store.task(foreign)["status"]=="running" &&
            store.task(foreign)["actions"][0]["status"]=="dispatched","scheduler recovered unrelated foreground task");
        const auto actual=store.task(task); check(actual["status"]=="interrupted","associated task not recovered");
        if (selected=="dispatched") check(store.schedule(id(job))["status"]=="blocked" && actual["actions"][0]["status"]=="unknown","unknown effect automatically retried");
        else {
            check(store.schedule(id(job))["status"]=="queued","safe tools checkpoint did not recover");
            if (selected=="prepared") check(actual["actions"][0]["status"]=="not_executed","prepared intent replayable after recovery");
            else check(actual["actions"][0]["result"]["output"]["marker"]=="durable receipt","recovery rewrote committed receipt");
            const auto resumed=store.claim_schedule(id(job),"new",next,now()); store.resume_task(task,binding(),id(job),token(resumed));
            check(store.task(task)["actions"].size()==1,"recovery created duplicate action");
        }
    }
}

#ifdef _WIN32
void repeated_proposals_require_each_decision() {
    Temp temp; Store store(temp.root/"state.sqlite3");
    const auto task=store.create_task("review each proposal","fixture-workspace","fixture");
    const Json arguments{{"manifest",{{"version",2}}}};
    const Json proposal{{"manifest",{{"version",2}}}};
    const auto digest=sha256_bytes(proposal.dump()); auto output=proposal; output["digest"]=digest;
    auto saved=state("tools");
    saved["pending_calls"]=Json::array({{{"id","proposal_1"},{"name","propose_build"},{"arguments",arguments}},
        {{"id","proposal_2"},{"name","propose_build"},{"arguments",arguments}}});
    store.save_checkpoint(task,saved,binding());
    const auto first=store.prepare_action(task,{"proposal_1","propose_build",arguments});
    store.dispatch_action(first); store.finish_action(first,{true,output,"","none"},&saved);
    store.finish_task(task,"waiting_approval","first review",&saved);
    const auto job=enroll(store,task); const auto generation=store.start_scheduler("owner",1,now());
    check(store.cancel_schedule(id(job),now(),true)==job,"active-only cancellation changed waiting job");
    const auto initial_events=store.events(task);
    for (int i=0;i<3;++i) check(store.claim_schedule(id(job),"owner",generation,now()).is_null(),"undecided proposal claimed");
    check(store.schedule(id(job))["attempts"]==0 && store.events(task)==initial_events,"approval polling consumed attempts or events");
    store.decide_build_proposal(task,first,digest,false,"fixture-workspace");
    auto claim=store.claim_schedule(id(job),"owner",generation,now()); store.resume_task(task,binding(),id(job),token(claim));
    auto body=store.proposal_continuation(task,first).at("result");
    saved["messages"].push_back({{"role","tool"},{"tool_call_id","proposal_1"},{"content",body.dump()}}); saved["next_call"]=1;
    check(store.consume_build_proposal(task,first,body,saved),"first decision did not consume");
    const auto second=store.prepare_action(task,{"proposal_2","propose_build",arguments});
    store.dispatch_action(second); store.finish_action(second,{true,output,"","none"},&saved);
    store.finish_task(task,"waiting_approval","second review",&saved);
    const auto waiting=store.schedule(id(job));
    check(waiting["status"]=="waiting_approval" && waiting["attempts"]==1 &&
        waiting["expected_revision"]==store.checkpoint_revision(task),"new proposal lost original job authorization");
    check(store.claim_schedule(id(job),"owner",generation,now()).is_null(),"first decision authorized second proposal");
    check(store.schedule(id(job))["attempts"]==1,"second proposal polling consumed attempts");
    store.decide_build_proposal(task,second,digest,false,"fixture-workspace");
    claim=store.claim_schedule(id(job),"owner",generation,now()); store.resume_task(task,binding(),id(job),token(claim));
    body=store.proposal_continuation(task,second).at("result");
    saved["messages"].push_back({{"role","tool"},{"tool_call_id","proposal_2"},{"content",body.dump()}}); saved["next_call"]=2;
    check(store.consume_build_proposal(task,second,body,saved),"second decision did not consume");
    saved["phase"]="finished"; saved["resumable"]=false; store.finish_task(task,"completed","done",&saved);
    check(store.schedule(id(job))["status"]=="completed" && store.schedule(id(job))["attempts"]==2 &&
        store.schedules()["items"].size()==1,"proposal continuation duplicated job or spent polling attempts");
}
#endif
}

void run_scheduler_store_tests() {
    supervisor_lifecycle_and_events(); supervisor_atomicity_and_ownership(); supervisor_input_boundaries();
    {
        Temp temp; Store store(temp.root/"state.sqlite3");
        const auto initial=store.scheduler();
        const auto epoch=initial.at("stop_epoch").get<std::int64_t>();
        const auto generation=initial.at("generation").get<std::int64_t>();
        const auto next=store.start_scheduler("supervisor",1,now(),SchedulerStartFence{epoch,generation});
        check(next==generation+1,"fenced start did not advance generation");
        const auto running=store.scheduler();
        rejects([&]{store.start_scheduler("stale",2,now(),SchedulerStartFence{epoch,generation});},"stale generation started");
        check(store.scheduler()==running,"stale child registration mutated control");
        check(store.stop_scheduler(SchedulerStartFence{epoch,generation})==running,"stale supervisor stopped newer worker");
        store.stop_scheduler();
        const auto stopped=store.scheduler();
        rejects([&]{store.start_scheduler("restart",2,now(),SchedulerStartFence{epoch,next});},"stop during launch resurrected worker");
        check(store.scheduler()==stopped,"stale epoch registration mutated control");
    }
    enrollment_and_readers(); atomic_claim_resume_finish(); retry_budgets_and_deadlines();
    fencing_cancellation_and_manual_resume(); recovery_is_scoped_and_preserves_receipts();
#ifdef _WIN32
    repeated_proposals_require_each_decision();
#endif
}
