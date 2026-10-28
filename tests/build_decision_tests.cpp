#include "codestudio/store.hpp"
#include "codestudio/state_reader.hpp"
#include "codestudio/digest.hpp"
#include <sqlite3.h>
#include <stdexcept>

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
namespace {
using namespace codestudio;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("build decisions: ") + message);
}
template<class F> void rejects(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected,message);
}
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("codestudio-build-decisions-" + new_id());
    Temp() { std::filesystem::create_directory(root); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root,error); }
};
struct Database {
    sqlite3* db = nullptr;
    explicit Database(const std::filesystem::path& path) {
        if (sqlite3_open(path_utf8(path).c_str(),&db) != SQLITE_OK) {
            sqlite3_close(db); db = nullptr;
            throw std::runtime_error("decision fixture database open failed");
        }
    }
    ~Database() { sqlite3_close(db); }
    void exec(const std::string& command) {
        char* error = nullptr;
        const auto code = sqlite3_exec(db,command.c_str(),nullptr,nullptr,&error);
        const std::string message = error ? error : "decision fixture SQL failed";
        sqlite3_free(error);
        if (code != SQLITE_OK) throw std::runtime_error(message);
    }
    int count(const char* command) {
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(db,command,-1,&statement,nullptr) == SQLITE_OK,"prepare count");
        const auto code = sqlite3_step(statement);
        const auto result = sqlite3_column_int(statement,0);
        sqlite3_finalize(statement);
        check(code == SQLITE_ROW,"read count");
        return result;
    }
};
// Initialized by the test entrypoint so unsupported hash backends report a
// normal test failure, rather than throwing during static initialization.
std::string digest;
struct Source { std::string task, action; };
Source propose(Store& store, const std::string& workspace, bool finish = true) {
    const auto task = store.create_task("propose only",workspace,"fixture");
    const auto action = store.prepare_action(task,{"proposal_1","propose_build",
        {{"manifest",{{"version",2}}},{"reason","review before running"}}});
    store.dispatch_action(action);
    store.finish_action(action,{true,{{"digest",digest},{"manifest",{{"version",2}}}},"","none"});
    if (finish) store.finish_task(task,"completed","proposal ready");
    return {task,action};
}
struct WaitingSource {
    std::string task,action;
    Json state,binding;
};
WaitingSource wait_for_proposal(Store& store, const std::string& workspace, bool second_call = false) {
    WaitingSource source;
    source.task = store.create_task("wait for approval",workspace,"fixture");
    const Json arguments{{"manifest",{{"version",2}}},{"reason","review before running"}};
    Json calls = Json::array({{{"id","proposal_1"},{"name","propose_build"},{"arguments",arguments}}});
    if (second_call) calls.push_back({{"id","proposal_2"},{"name","propose_build"},{"arguments",arguments}});
    Json native_calls = Json::array();
    for (const auto& call : calls)
        native_calls.push_back({{"id",call.at("id")},{"type","function"},
            {"function",{{"name","propose_build"},{"arguments",arguments.dump()}}}});
    source.state = {{"version",2},{"phase","tools"},{"resumable",true},{"blocked_reason",""},
        {"messages",Json::array({{{"role","system"},{"content","fixture"}},
            {{"role","user"},{"content","wait for approval"}},
            {{"role","assistant"},{"content",nullptr},{"tool_calls",native_calls}}})},
        {"pending_calls",calls},{"next_call",0},{"turns",1},{"max_turns",8},{"tool_failed",false},
        {"calls_seen",second_call ? Json::array({"proposal_1","proposal_2"}) : Json::array({"proposal_1"})}};
    source.binding = {{"version",2},{"provider","fixture"},{"workspace",workspace}};
    store.save_checkpoint(source.task,source.state,source.binding);
    source.action = store.prepare_action(source.task,{"proposal_1","propose_build",arguments});
    store.dispatch_action(source.action);
    store.finish_action(source.action,{true,{{"digest",digest},{"manifest",{{"version",2}}}},"","none"},&source.state);
    store.finish_task(source.task,"waiting_approval","Waiting for review",&source.state);
    return source;
}
Json append_continuation(Json state,const Json& body) {
    const auto cursor = state.at("next_call").get<std::size_t>();
    state["messages"].push_back({{"role","tool"},{"tool_call_id",state.at("pending_calls")[cursor].at("id")},
        {"content",body.dump()}});
    state["next_call"] = cursor + 1;
    return state;
}
void atomic_decisions_and_recovery() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; const auto workspace=path_utf8(temp.root);
    Store store(path); StateReader reader(path); Database raw(path);
    const auto source=propose(store,workspace);
    check(store.build_proposal(source.task,source.action)==reader.build_proposal(source.task,source.action),"reader differs from writer");
    rejects([&]{ store.decide_build_proposal(source.task,source.action,std::string(64,'b'),true,workspace); },"changed digest accepted");
    rejects([&]{ store.decide_build_proposal(source.task,source.action,digest,true,"different workspace"); },"different workspace accepted");
    for (const auto& invalid : {std::string(64,'A'), std::string(63,'a'), std::string(64,'z')})
        rejects([&]{ store.decide_build_proposal(source.task,source.action,invalid,true,workspace); },"invalid digest accepted");
    const auto before_tasks=raw.count("SELECT count(*) FROM tasks"), before_actions=raw.count("SELECT count(*) FROM actions");
    const auto before_events=store.events(source.task);
    // Fail the final audit insert, after task/action/decision inserts were made.
    raw.exec("CREATE TRIGGER fail_decision_event BEFORE INSERT ON events WHEN NEW.kind='build.proposal.approved' BEGIN SELECT RAISE(ABORT,'injected decision failure'); END;");
    rejects([&]{ store.decide_build_proposal(source.task,source.action,digest,true,workspace); },"event failure did not abort approval");
    check(raw.count("SELECT count(*) FROM tasks")==before_tasks && raw.count("SELECT count(*) FROM actions")==before_actions &&
        raw.count("SELECT count(*) FROM build_proposal_decisions")==0 && store.events(source.task)==before_events,
        "approval failure leaked task, action, decision or audit event");
    raw.exec("DROP TRIGGER fail_decision_event");
    const auto first=store.decide_build_proposal(source.task,source.action,digest,true,workspace);
    check(first["decision_created"]==true,"first decision not newly created");
    const auto execution_task=first["decision"]["execution_task_id"].get<std::string>();
    const auto execution_action=first["decision"]["execution_action_id"].get<std::string>();
    const auto execution=store.task(execution_task);
    check(execution["provider"]=="approved-build" && execution["status"]=="running" &&
        execution["actions"].size()==1 && execution["actions"][0]["status"]=="prepared", "approval executed or lost prepared intent");
    const auto repeated=store.decide_build_proposal(source.task,source.action,digest,true,workspace);
    check(repeated["decision_created"]==false && repeated["decision"]==first["decision"],"repeat approval created another execution");
    rejects([&]{ store.decide_build_proposal(source.task,source.action,digest,false,workspace); },"approved proposal changed to rejected");
    store.dispatch_action(execution_action);
    store.recover();
    check(store.task(execution_task)["actions"][0]["status"]=="unknown","dispatched approval not recovered as unknown");
    check(store.decide_build_proposal(source.task,source.action,digest,true,workspace)["decision_created"]==false,
        "recovery released an approved proposal for replay");
    check(raw.count("SELECT count(*) FROM tasks")==before_tasks+1 && raw.count("SELECT count(*) FROM actions")==before_actions+1,
        "repeat approval changed execution count");
    check(reader.build_proposal(source.task,source.action)["decision"]==first["decision"],"committed decision not readable");
    rejects([&]{ store.checkpoint(execution_task); },"unrequested resume checkpoint created");

    const auto undispatched=propose(store,workspace);
    const auto claim=store.decide_build_proposal(undispatched.task,undispatched.action,digest,true,workspace);
    const auto undispatched_task=claim["decision"]["execution_task_id"].get<std::string>();
    store.recover();
    check(store.task(undispatched_task)["actions"][0]["status"]=="not_executed","prepared approval became unknown");
    check(store.decide_build_proposal(undispatched.task,undispatched.action,digest,true,workspace)["decision_created"]==false,
        "prepared recovery allowed a second claim");
}
void rejection_and_source_validation() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; const auto workspace=path_utf8(temp.root);
    Store store(path); Database raw(path); StateReader reader(path);
    const auto source=propose(store,workspace,false);
    check(reader.build_proposal(source.task,source.action)["decision"].is_null(),"running source is not reviewable");
    rejects([&]{ store.decide_build_proposal(source.task,source.action,digest,false,workspace); },"decision allowed during active task");
    store.finish_task(source.task,"completed","ready");
    const auto rejected=store.decide_build_proposal(source.task,source.action,digest,false,workspace);
    check(rejected["decision"]["decision"]=="rejected" && rejected["decision"]["execution_task_id"].is_null() &&
        rejected["decision"]["execution_action_id"].is_null(),"rejection created execution");
    check(store.decide_build_proposal(source.task,source.action,digest,false,workspace)["decision_created"]==false,"rejection not idempotent");
    rejects([&]{ store.decide_build_proposal(source.task,source.action,digest,true,workspace); },"rejected proposal was approved");
    check(raw.count("SELECT count(*) FROM tasks")==1 && raw.count("SELECT count(*) FROM actions")==1,"rejection changed task count");
    rejects([&]{ store.build_proposal("other-task",source.action); },"action selected through wrong source task");
    for (const auto& change : {"tool='build_workspace'", "status='failed'", "effect='unknown'",
                              "result='{\"ok\":false,\"effect\":\"none\",\"output\":{}}'"}) {
        const auto candidate=propose(store,workspace);
        raw.exec("UPDATE actions SET " + std::string(change) + " WHERE id='" + candidate.action + "'");
        rejects([&]{ store.build_proposal(candidate.task,candidate.action); },"invalid source accepted by Store");
        rejects([&]{ reader.build_proposal(candidate.task,candidate.action); },"invalid source accepted by reader");
    }
}
void waiting_and_atomic_consumption() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; const auto workspace=path_utf8(temp.root);
    std::string task,action;
    Json body,advanced;
    {
        Store store(path); StateReader reader(path); Database raw(path);
        const auto source=wait_for_proposal(store,workspace); task=source.task; action=source.action;
        const auto original=store.task(task), before=store.events(task);
        check(original.at("status")=="waiting_approval" && reader.task(task).at("status")=="waiting_approval" &&
            reader.task(task).at("checkpoint").at("phase")=="tools","waiting state is not visible to readers");
        check(before.back().at("kind")=="task.waiting_approval","pause emitted a terminal task event");
        check(original.at("actions")[0].at("continuation").at("status")=="pending","undecided proposal is not pending");
        check(original.at("actions")[0].at("continuation_consumed")==false,"unconsumed proposal acquired a consumption marker");
        rejects([&] { store.resume_task(task,source.binding); },"pending approval resumed");
        check(store.task(task)==original && store.events(task)==before,"pending resume changed durable state");
        store.decide_build_proposal(task,action,digest,false,workspace);
        body=store.proposal_continuation(task,action).at("result");
        check(body.at("ok")==true && body.at("effect")=="none" && body.at("output").at("execution").is_null() &&
            body.at("output").at("decision").at("decision")=="rejected","rejection is not a side-effect-free continuation");
        auto next=append_continuation(source.state,body);
        rejects([&] { store.consume_build_proposal(task,action,body,next); },"paused task consumed a result without resuming");
        store.resume_task(task,source.binding);
        const auto checkpoint=store.checkpoint(task), events=store.events(task);
        auto changed=body; changed["output"]["decision"]["decision"]="approved";
        rejects([&] { store.consume_build_proposal(task,action,changed,next); },"changed continuation was consumed");
        auto wrong_cursor=next; wrong_cursor["next_call"]=2;
        rejects([&] { store.consume_build_proposal(task,action,body,wrong_cursor); },"consumption skipped a tool call");
        auto wrong_budget=next; wrong_budget["turns"]=0;
        rejects([&] { store.consume_build_proposal(task,action,body,wrong_budget); },"consumption reset the model budget");
        raw.exec("CREATE TRIGGER fail_consumed_event BEFORE INSERT ON events WHEN NEW.kind='build.proposal.consumed' "
            "BEGIN SELECT RAISE(ABORT,'injected consume failure'); END;");
        rejects([&] { store.consume_build_proposal(task,action,body,next); },"consume event failure did not abort");
        check(store.checkpoint(task)==checkpoint && store.events(task)==events,"failed consumption advanced checkpoint or event stream");
        raw.exec("DROP TRIGGER fail_consumed_event");
        check(store.consume_build_proposal(task,action,body,next),"first continuation was not consumed");
        check(store.checkpoint(task).at("state")==next && store.events(task).back().at("kind")=="build.proposal.consumed",
            "checkpoint and consumption marker did not commit together");
        check(store.task(task).at("actions")[0].at("result")==original.at("actions")[0].at("result"),
            "continuation rewrote the original proposal receipt");
        check(store.task(task).at("actions")[0].at("continuation_consumed")==true,"consumed proposal annotation was not durable");
        advanced=next; advanced["pending_calls"]=Json::array(); advanced["next_call"]=0; advanced["phase"]="ready";
        store.save_checkpoint(task,advanced,source.binding);
        const auto once=store.events(task);
        check(!store.consume_build_proposal(task,action,body,next),"duplicate consumption was not idempotent");
        check(store.checkpoint(task).at("state")==advanced && store.events(task)==once,"duplicate consumption rewound newer state");
        store.finish_task(task,"exhausted","fixture stop");
    }
    {
        Store reopened(path); Database raw(path);
        check(!reopened.consume_build_proposal(task,action,body,Json::object()),"reopen forgot consumed proposal");
        check(reopened.checkpoint(task).at("state")==advanced &&
            raw.count("SELECT count(*) FROM events WHERE kind='build.proposal.consumed'")==1,
            "reopen duplicated continuation or changed checkpoint");
    }
}
void execution_outcomes_and_recovery() {
    for (const auto* mode : {"success","failure","receipt-only","not-executed","unknown","partial"}) {
        Temp temp; const auto path=temp.root/"state.sqlite3"; const auto workspace=path_utf8(temp.root);
        Store store(path); const auto source=wait_for_proposal(store,workspace);
        const auto claimed=store.decide_build_proposal(source.task,source.action,digest,true,workspace);
        const auto execution_task=claimed.at("decision").at("execution_task_id").get<std::string>();
        const auto execution_action=claimed.at("decision").at("execution_action_id").get<std::string>();
        check(store.proposal_continuation(source.task,source.action).at("status")=="pending","prepared execution was ready");
        rejects([&] { store.resume_task(source.task,source.binding); },"pending executor resumed source task");
        const std::string selected=mode;
        if (selected=="not-executed") store.recover();
        else {
            store.dispatch_action(execution_action);
            check(store.proposal_continuation(source.task,source.action).at("status")=="pending","dispatched execution was ready");
            if (selected=="unknown") store.recover();
            else {
                const bool success=selected=="success" || selected=="receipt-only";
                const std::string effect=selected=="partial" ? "partial" : success ? "applied" : "none";
                store.finish_action(execution_action,{success,{{"marker","recorded build receipt"}},success ? "" : "build failed",effect});
                if (selected=="receipt-only") {
                    check(store.proposal_continuation(source.task,source.action).at("status")=="pending",
                        "receipt-only running task bypassed explicit recovery");
                    store.recover();
                } else store.finish_task(execution_task,selected=="partial" ? "needs_attention" : success ? "completed" : "failed","build finished");
            }
        }
        const auto continuation=store.proposal_continuation(source.task,source.action);
        if (selected=="unknown" || selected=="partial") {
            check(continuation.at("status")=="blocked" && !continuation.contains("result"),"uncertain execution yielded a consumable result");
            const auto before=store.events(source.task);
            rejects([&] { store.resume_task(source.task,source.binding); },"uncertain executor resumed source task");
            check(store.events(source.task)==before && store.task(source.task).at("status")=="waiting_approval",
                "uncertain resume changed source task");
        } else {
            check(continuation.at("status")=="ready","known execution outcome was not ready");
            const auto& execution=continuation.at("result").at("output").at("execution");
            check(execution.at("task_id")==execution_task && execution.at("action_id")==execution_action,
                "continuation lost execution identity");
            if (selected=="not-executed") check(execution.at("status")=="not_executed" && execution.at("result").at("effect")=="none",
                "undispatched recovery lost its no-effect result");
            else check(execution.at("result").at("output").at("marker")=="recorded build receipt",
                "known build receipt was not reused");
            store.resume_task(source.task,source.binding);
            check(store.consume_build_proposal(source.task,source.action,continuation.at("result"),
                append_continuation(source.state,continuation.at("result"))),"known outcome was not consumed");
            store.finish_task(source.task,"exhausted","fixture stop");
        }
        check(store.decide_build_proposal(source.task,source.action,digest,true,workspace).at("decision_created")==false,
            "continuation or recovery released approval for replay");
    }
}
void waiting_guards_and_multiple_proposals() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; const auto workspace=path_utf8(temp.root);
    Store store(path); const auto source=wait_for_proposal(store,workspace,true);
    store.decide_build_proposal(source.task,source.action,digest,false,workspace);
    const auto first=store.proposal_continuation(source.task,source.action).at("result");
    store.resume_task(source.task,source.binding);
    auto state=append_continuation(source.state,first);
    store.consume_build_proposal(source.task,source.action,first,state);
    rejects([&] { store.finish_task(source.task,"waiting_approval","missing checkpoint"); },"waiting without checkpoint accepted");
    rejects([&] { store.finish_task(source.task,"waiting_approval","no second receipt",&state); },"waiting for unexecuted proposal accepted");
    const auto& call=state.at("pending_calls")[1];
    const auto second=store.prepare_action(source.task,{call.at("id").get<std::string>(),"propose_build",call.at("arguments")});
    rejects([&] { store.finish_task(source.task,"waiting_approval","prepared",&state); },"prepared proposal entered waiting state");
    store.dispatch_action(second);
    store.finish_action(second,{true,{{"digest",digest},{"manifest",{{"version",2}}}},"","none"},&state);
    store.finish_task(source.task,"waiting_approval","second review",&state);
    check(state.at("turns")==1 && store.checkpoint(source.task).at("state").at("next_call")==1,
        "second proposal reset same-turn cursor or budget");
    store.decide_build_proposal(source.task,second,digest,false,workspace);
    const auto next=store.proposal_continuation(source.task,second).at("result");
    store.resume_task(source.task,source.binding);
    state=append_continuation(state,next);
    check(store.consume_build_proposal(source.task,second,next,state),"second proposal was not consumed independently");
    check(state.at("next_call")==2 && state.at("messages").size()==5 && state.at("turns")==1,
        "multiple proposals did not preserve ordered tool history and budget");
}
void continuation_association_guards() {
    for (const auto* mutation : {"workspace","provider","call_id","tool","arguments","digest","result","oversized"}) {
        Temp temp; const auto path=temp.root/"state.sqlite3"; const auto workspace=path_utf8(temp.root);
        Store store(path); Database raw(path); const auto source=wait_for_proposal(store,workspace);
        const auto claimed=store.decide_build_proposal(source.task,source.action,digest,true,workspace);
        const auto task=claimed.at("decision").at("execution_task_id").get<std::string>();
        const auto action=claimed.at("decision").at("execution_action_id").get<std::string>();
        store.dispatch_action(action); store.finish_action(action,{true,Json::object(),"","applied"});
        store.finish_task(task,"completed","done");
        const std::string field=mutation;
        if (field=="workspace" || field=="provider") raw.exec("UPDATE tasks SET " + field + "='wrong' WHERE id='" + task + "'");
        else if (field=="digest") raw.exec("UPDATE build_proposal_decisions SET digest='" + std::string(64,'b') + "'");
        else if (field=="oversized") raw.exec("UPDATE actions SET result=zeroblob(16777217) WHERE id='" + action + "'");
        else raw.exec("UPDATE actions SET " + field + "='wrong' WHERE id='" + action + "'");
        const auto before=store.events(source.task);
        rejects([&] { (void)store.proposal_continuation(source.task,source.action); },"invalid execution association or receipt accepted");
        rejects([&] { store.resume_task(source.task,source.binding); },"invalid execution association resumed source");
        check(store.events(source.task)==before,"invalid association changed source audit history");
    }
}
void reviewed_proposal_content_is_immutable() {
    Temp temp; const auto path=temp.root/"state.sqlite3"; const auto workspace=path_utf8(temp.root);
    Store store(path); Database raw(path); const auto source=wait_for_proposal(store,workspace);
    const auto claimed=store.decide_build_proposal(source.task,source.action,digest,true,workspace);
    const auto task=claimed.at("decision").at("execution_task_id").get<std::string>();
    const auto action=claimed.at("decision").at("execution_action_id").get<std::string>();
    store.dispatch_action(action); store.finish_action(action,{true,Json::object(),"","applied"});
    store.finish_task(task,"completed","done");
    const auto original=store.build_proposal(source.task,source.action).at("result");
    const auto continuation=store.proposal_continuation(source.task,source.action).at("result");
    const auto checkpoint=store.checkpoint(source.task), events=store.events(source.task);
    for (const auto* mutation : {"manifest","reason","recipe"}) {
        auto changed=original;
        if (std::string(mutation)=="manifest") changed["output"]["manifest"]["version"]=3;
        else if (std::string(mutation)=="reason") changed["output"]["reason"]="altered after approval";
        else changed["output"]["recipe"]={{"version",2},{"steps",Json::array()}};
        check(changed.at("output").at("digest")==digest,"tamper fixture changed the reviewed digest");
        raw.exec("UPDATE actions SET result='" + changed.dump() + "' WHERE id='" + source.action + "'");
        rejects([&] { (void)store.proposal_continuation(source.task,source.action); },"changed proposal content reused its old digest");
        rejects([&] { store.resume_task(source.task,source.binding); },"changed proposal content resumed the source");
        rejects([&] { store.consume_build_proposal(source.task,source.action,continuation,
            append_continuation(source.state,continuation)); },"changed proposal content was consumed");
        check(store.checkpoint(source.task)==checkpoint && store.events(source.task)==events &&
            store.list_tasks().back().at("status")=="waiting_approval",
            "rejected proposal tampering changed task, checkpoint, or audit history");
    }
    raw.exec("UPDATE actions SET result='" + original.dump() + "' WHERE id='" + source.action + "'");
    check(store.proposal_continuation(source.task,source.action).at("result")==continuation,
        "unchanged reviewed proposal no longer produced its original continuation");
}
}
#endif
void run_build_decision_tests() {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    digest=sha256_bytes(Json{{"manifest",{{"version",2}}}}.dump());
    atomic_decisions_and_recovery();
    rejection_and_source_validation();
    waiting_and_atomic_consumption();
    execution_outcomes_and_recovery();
    waiting_guards_and_multiple_proposals();
    continuation_association_guards();
    reviewed_proposal_content_is_immutable();
#endif
}
