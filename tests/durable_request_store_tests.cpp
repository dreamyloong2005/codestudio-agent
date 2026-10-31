#include "codestudio/kernel.hpp"
#include "codestudio/state_reader.hpp"
#include "codestudio/digest.hpp"
#include <sqlite3.h>
#include <array>
#include <barrier>
#include <exception>
#include <fstream>
#include <functional>
#include <thread>

namespace {
using namespace codestudio;
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class Error=std::exception,class F> void rejects(F&& operation,const char* message) {
    bool rejected=false;
    try { operation(); } catch (const Error&) { rejected=true; }
    check(rejected,message);
}
struct Temp {
    std::filesystem::path root=std::filesystem::temp_directory_path()/("codestudio-requests-"+new_id());
    Temp() { std::filesystem::create_directories(root); root=std::filesystem::canonical(root); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root,error); }
};
class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        check(sqlite3_open(path_utf8(path).c_str(),&db_)==SQLITE_OK,"Cannot open request test database");
        sqlite3_busy_timeout(db_,5000);
    }
    ~Database() { sqlite3_close(db_); }
    void exec(const std::string& command) {
        char* error=nullptr;
        const auto result=sqlite3_exec(db_,command.c_str(),nullptr,nullptr,&error);
        const std::string message=error ? error : "Request fixture SQL failed";
        sqlite3_free(error);
        if (result!=SQLITE_OK) throw std::runtime_error(message);
    }
private:
    sqlite3* db_=nullptr;
};
DurableRequest admission(Store& store,const Temp& temp,const std::string& key="request-1",
                         const std::string& method="tasks/start") {
    return {store.database_id(),key,sha256_bytes("frozen parameters and grants"),method,
            new_id(),new_id(),path_utf8(temp.root)};
}
Json initial_state() { return {{"version",2},{"phase","ready"},{"resumable",true}}; }
Json binding(const Temp& temp) { return {{"version",2},{"workspace",path_utf8(temp.root)}}; }

void admission_persists_and_conflicts() {
    Temp temp; const auto path=temp.root/"state.sqlite3";
    DurableRequest original;
    Json receipt;
    {
        Store store(path); original=admission(store,temp);
        receipt=store.admit_request(original);
        check(receipt.at("created")==true && receipt.at("state")=="incomplete" &&
              receipt.at("task_id").is_null() && receipt.at("completion").is_null(),"Admission pretended to complete work");
        receipt.erase("created");
        check(store.request(original.request_key)==receipt,"Stored request differs from admission");
        StateReader reader(path);
        check(reader.request(original.database_id,original.request_key,original.request_digest,
                             original.method,original.workspace)==receipt,"Read-only admission view differs");
        auto duplicate=original; duplicate.operation_id=new_id(); duplicate.session_id=new_id();
        auto repeated=store.admit_request(duplicate);
        check(repeated.at("created")==false,"Repeated request was admitted again");
        repeated.erase("created"); check(repeated==receipt,"Repeat changed the original operation identity");
        for (int mutation=0;mutation<3;++mutation) {
            auto conflicting=original;
            if (mutation==0) conflicting.request_digest=sha256_bytes("changed authorization");
            if (mutation==1) conflicting.method="tasks/resume";
            if (mutation==2) conflicting.workspace+="-different";
            rejects<RequestConflict>([&] { store.admit_request(conflicting); },"Conflicting key was reused");
            rejects<RequestConflict>([&] { reader.request(original.database_id,original.request_key,
                conflicting.request_digest,conflicting.method,conflicting.workspace); },"Read-only duplicate bypassed request binding");
        }
        auto wrong_database=original; wrong_database.database_id=new_id();
        rejects<DatabaseMismatch>([&] { store.admit_request(wrong_database); },"Admission accepted replaced database");
        check(store.list_tasks().empty(),"Admission or lookup created a task");
    }
    {
        Store reopened(path); StateReader reader(path);
        check(reopened.database_id()==original.database_id && reader.database_id()==original.database_id,
              "Database identity changed on reopen");
        check(reader.request(original.database_id,original.request_key)==receipt,"Restart lost incomplete admission");
        check(reopened.admit_request(original).at("created")==false,"Restart re-admitted incomplete request");
        check(reader.request(original.database_id,"unknown").is_null(),"Absent request fabricated a receipt");
    }
}

void creation_and_resume_are_atomic() {
    Temp temp; Store store(temp.root/"state.sqlite3"); Database raw(temp.root/"state.sqlite3");
    const auto start=admission(store,temp); store.admit_request(start);
    const auto state=initial_state(), grant=binding(temp);
    raw.exec("CREATE TRIGGER refuse_checkpoint BEFORE INSERT ON task_checkpoints BEGIN SELECT RAISE(ABORT,'checkpoint fixture'); END;");
    rejects([&] { store.create_task("atomic",start.workspace,"fixture",&state,&grant,start.operation_id); },
            "Injected checkpoint failure was ignored");
    check(store.list_tasks().empty() && store.request(start.request_key).at("task_id").is_null(),
          "Checkpoint failure left task or request association");
    raw.exec("DROP TRIGGER refuse_checkpoint;");
    const auto task=store.create_task("atomic",start.workspace,"fixture",&state,&grant,start.operation_id);
    check(store.checkpoint(task).at("state")==state && store.request(start.request_key).at("task_id")==task,
          "Task, initial checkpoint and request association did not commit together");
    rejects<RequestConflict>([&] { store.create_task("duplicate",start.workspace,"fixture",&state,&grant,start.operation_id); },
                             "One request created two tasks");
    check(store.list_tasks().size()==1,"Failed second association leaked a task");
    store.finish_task(task,"cancelled","fixture pause");
    const auto resume=admission(store,temp,"resume-1","tasks/resume"); store.admit_request(resume);
    const auto before=store.events(task);
    raw.exec("CREATE TRIGGER refuse_association BEFORE UPDATE OF task_id ON durable_requests WHEN NEW.task_id IS NOT NULL BEGIN SELECT RAISE(ABORT,'association fixture'); END;");
    rejects([&] { store.resume_task(task,grant,{},{},resume.operation_id); },"Injected resume association failure ignored");
    check(store.task(task).at("status")=="cancelled" && store.events(task)==before &&
          store.request(resume.request_key).at("task_id").is_null(),"Failed resume association committed transition");
    raw.exec("DROP TRIGGER refuse_association;");
    store.resume_task(task,grant,{},{},resume.operation_id);
    check(store.task(task).at("status")=="running" && store.request(resume.request_key).at("task_id")==task,
          "Resume and request association did not commit together");
    store.finish_task(task,"cancelled","fixture pause again");
    rejects<RequestConflict>([&] { store.resume_task(task,grant,{},{},resume.operation_id); },"Resume request entered runner twice");
    check(store.task(task).at("status")=="cancelled","Repeated resume partially changed task status");
    store.resume_task(task,grant);
    const auto action=store.prepare_action(task,{"uncertain-call","counter",Json::object()});
    store.dispatch_action(action); store.recover();
    const auto uncertain=admission(store,temp,"resume-uncertain","tasks/resume"); store.admit_request(uncertain);
    rejects([&] { store.resume_task(task,grant,{},{},uncertain.operation_id); },"Request association bypassed unknown-effect refusal");
    check(store.request(uncertain.request_key).at("task_id").is_null() &&
          store.task(task).at("actions").at(0).at("effect")=="unknown","Unknown-effect refusal changed request or action");
}

void completion_is_durable_and_immutable() {
    Temp temp; const auto path=temp.root/"state.sqlite3";
    DurableRequest value;
    const Json completion{{"exit_code",2},{"error",{{"message","Fixture failure"}}}};
    {
        Store store(path); Database raw(path); value=admission(store,temp); store.admit_request(value);
        raw.exec("CREATE TRIGGER refuse_completion BEFORE UPDATE ON durable_requests WHEN NEW.completion IS NOT NULL BEGIN SELECT RAISE(ABORT,'completion fixture'); END;");
        rejects([&] { store.finish_request(value.operation_id,completion); },"Completion persistence failure was ignored");
        check(store.request(value.request_key).at("state")=="incomplete" &&
              store.request(value.request_key).at("completion").is_null(),"Failed completion became observable");
        raw.exec("DROP TRIGGER refuse_completion;");
        store.finish_request(value.operation_id,completion);
        store.finish_request(value.operation_id,completion);
        rejects<RequestConflict>([&] { store.finish_request(value.operation_id,{{"exit_code",0}}); },
                                 "Different result overwrote immutable completion");
        rejects([&] { store.finish_request(value.operation_id,{{"value",std::string(300*1024,'x')}}); },
                "Oversized durable completion accepted");
        check(store.request(value.request_key).at("completion")==completion,"Immutable receipt changed");
    }
    StateReader reader(path);
    const auto receipt=reader.request(value.database_id,value.request_key);
    check(receipt.at("state")=="finished" && receipt.at("completion")==completion,"Restart lost final completion");
}

void readers_preserve_missing_state_and_detect_replacement() {
    Temp temp; const auto absent=temp.root/"absent.sqlite3";
    StateReader missing(absent);
    check(missing.database_id().is_null(),"Missing database fabricated identity");
    rejects<DatabaseMismatch>([&] { missing.request(new_id(),"absent-request"); },"Missing database accepted old identity");
    check(!std::filesystem::exists(absent),"Read-only request query created missing database");
    const auto path=temp.root/"state.sqlite3";
    std::string previous;
    {
        Store store(path); previous=store.database_id();
    }
    StateReader reader(path);
    check(reader.database_id()==previous,"Reader missed initial database identity");
    Database raw(path);
    const auto replacement=new_id();
    raw.exec("UPDATE database_identity SET database_id='"+replacement+"';");
    check(reader.database_id()==replacement,"Reader cached previous database identity");
    rejects<DatabaseMismatch>([&] { reader.request(previous,"request-1"); },"Lookup accepted replaced database identity");
}

void concurrent_admission_has_one_winner() {
    Temp temp; const auto path=temp.root/"state.sqlite3";
    DurableRequest value;
    { Store initial(path); value=admission(initial,temp); }
    std::array<Json,2> receipts;
    std::array<std::exception_ptr,2> errors;
    std::barrier gate(2);
    std::array<std::thread,2> workers;
    for (std::size_t i=0;i<workers.size();++i) workers[i]=std::thread([&,i] {
        try {
            Store store(path);
            auto request=value; request.operation_id=new_id(); request.session_id=new_id();
            gate.arrive_and_wait();
            receipts[i]=store.admit_request(request);
        } catch (...) { errors[i]=std::current_exception(); gate.arrive_and_drop(); }
    });
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    check(receipts[0].at("created")!=receipts[1].at("created"),"Concurrent duplicate had zero or two winners");
    receipts[0].erase("created"); receipts[1].erase("created");
    check(receipts[0]==receipts[1],"Concurrent duplicate returned distinct operations");
}

void retention_limit_never_forgets_existing_keys() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; Store store(path); Database raw(path);
    const auto original=admission(store,temp); store.admit_request(original);
    raw.exec("WITH RECURSIVE slots(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM slots WHERE n<9999) "
        "INSERT INTO durable_requests(request_key,request_digest,method,operation_id,session_id,workspace) "
        "SELECT 'retained-'||n,lower(hex(zeroblob(32))),'tasks/start',printf('%032x',n),"
        "lower(hex(zeroblob(16))),'fixture-workspace' FROM slots;");
    const auto overflow=admission(store,temp,"overflow");
    rejects<RequestLimit>([&] { store.admit_request(overflow); },"Retention limit allowed new admission");
    check(store.admit_request(original).at("created")==false,"Retention limit lost original key");
    check(store.request("retained-1").at("state")=="incomplete" && store.request("overflow").is_null(),
          "Retention eviction or partial overflow admission occurred");
}

class ObservingProvider final : public ModelProvider {
public:
    int calls=0;
    std::function<void()> observe;
    std::string name() const override { return "request-fixture"; }
    ModelReply complete(const Json&,const Json&,const std::atomic_bool&) override {
        ++calls; if (observe) observe();
        return {"done",{},{{"role","assistant"},{"content","done"}},Json::object()};
    }
};
void kernel_associates_before_model_execution() {
    Temp temp; Store store(temp.root/"state.sqlite3");
    ToolPolicy policy; policy.workspace=temp.root; ToolBroker broker(policy);
    const auto start=admission(store,temp); store.admit_request(start);
    RunOptions options; options.prompt="durable kernel"; options.request_operation_id=start.operation_id;
    std::atomic_bool cancel{true}; ObservingProvider provider;
    const auto paused=run_agent(store,provider,broker,options,cancel);
    const auto task=paused.at("task_id").get<std::string>();
    check(provider.calls==0 && store.request(start.request_key).at("task_id")==task &&
          store.checkpoint(task).at("state").at("phase")=="ready","Cancelled initial admission lost atomic checkpoint");
    const auto resume=admission(store,temp,"kernel-resume","tasks/resume"); store.admit_request(resume);
    options.resume_task_id=task; options.request_operation_id=resume.operation_id;
    provider.observe=[&] {
        check(store.request(resume.request_key).at("task_id")==task,"Model ran before durable resume association");
    };
    cancel=false;
    const auto complete=run_agent(store,provider,broker,options,cancel);
    check(complete.at("status")=="completed" && provider.calls==1,"Durable kernel resume failed");
    rejects([&] { run_agent(store,provider,broker,options,cancel); },"Completed durable resume reentered kernel");
    check(provider.calls==1,"Duplicate resume called model again");
}
}

void run_durable_request_store_tests() {
    const std::pair<const char*,void(*)()> cases[] = {
        {"admission persistence/conflicts",admission_persists_and_conflicts},
        {"creation/resume atomicity",creation_and_resume_are_atomic},
        {"completion durability/immutability",completion_is_durable_and_immutable},
        {"read-only identity/replacement",readers_preserve_missing_state_and_detect_replacement},
        {"concurrent admission",concurrent_admission_has_one_winner},
        {"permanent retention limit",retention_limit_never_forgets_existing_keys},
        {"kernel association before execution",kernel_associates_before_model_execution}
    };
    for (const auto& [name,test] : cases) {
        try { test(); }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string("Durable request ")+name+": "+error.what());
        }
    }
}
