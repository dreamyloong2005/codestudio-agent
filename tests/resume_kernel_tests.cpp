#include "codestudio/kernel.hpp"
#include "codestudio/digest.hpp"
#include <sqlite3.h>
#include <functional>
#include <stdexcept>

namespace {
using namespace codestudio;
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& function,const char* message) {
    bool caught=false; try { function(); } catch (const std::exception&) { caught=true; }
    check(caught,message);
}
struct Temp {
    std::filesystem::path root=std::filesystem::temp_directory_path()/("codestudio-resume-"+new_id());
    Temp() { std::filesystem::create_directories(root); }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root,error); }
};
ModelReply call_reply(const std::vector<ToolCall>& calls) {
    Json native=Json::array();
    for (const auto& call : calls)
        native.push_back({{"id",call.id},{"type","function"},{"function",{{"name",call.name},{"arguments",call.arguments.dump()}}}});
    return {"",calls,{{"role","assistant"},{"content",nullptr},{"tool_calls",native}},Json::object()};
}
ModelReply done() { return {"done",{},{{"role","assistant"},{"content","done"}},Json::object()}; }
class Provider final : public ModelProvider {
public:
    std::vector<ModelReply> replies;
    std::size_t calls=0;
    std::function<void(const Json&)> observe;
    std::string name() const override { return "resume-fixture"; }
    ModelReply complete(const Json& history,const Json&,const std::atomic_bool&) override {
        if (observe) observe(history);
        const auto index=calls++;
        if (index>=replies.size()) throw std::runtime_error("Injected model request interruption");
        return replies[index];
    }
};
class Counter final : public ToolExtension {
public:
    mutable int executed=0;
    std::function<void()> after_effect;
    Json schemas() const override {
        return Json::array({{{"type","function"},{"function",{{"name","count_effect"},{"parameters",{{"type","object"}}}}}}});
    }
    void authorize(const ToolCall& call) const override {
        if (call.name!="count_effect" || !call.arguments.is_object()) throw std::runtime_error("Invalid counting tool");
    }
    ToolResult execute(const ToolCall&,const std::atomic_bool&) const override {
        ++executed;
        if (after_effect) after_effect();
        return {true,{{"executed",executed}},"","applied"};
    }
};
std::string proposal_digest() { return sha256_bytes(Json{{"manifest",{{"version",2}}}}.dump()); }
class Proposal final : public ToolExtension {
public:
    mutable int executed=0;
    Json schemas() const override {
        return Json::array({{{"type","function"},{"function",{{"name","propose_build"},{"parameters",{{"type","object"}}}}}}});
    }
    void authorize(const ToolCall& call) const override {
        if (call.name!="propose_build") throw std::runtime_error("Invalid proposal tool");
    }
    ToolResult execute(const ToolCall&,const std::atomic_bool&) const override {
        ++executed;
        return {true,{{"digest",proposal_digest()},{"manifest",{{"version",2}}}},"","none"};
    }
};
ToolCall count_call(const std::string& id) { return {id,"count_effect",Json::object()}; }
ToolPolicy policy(const Temp& temp) {
    ToolPolicy value; value.workspace=temp.root;
    value.protected_paths={temp.root/"state.sqlite3"}; return value;
}
RunOptions resume_options(const std::string& id,int turns=8) {
    RunOptions options; options.max_turns=turns; options.resume_task_id=id; return options;
}

void budget_and_continuation() {
    Temp temp; Store store(temp.root/"state.sqlite3"); std::atomic_bool cancel{false};
    auto counter=std::make_shared<Counter>(); ToolBroker broker(policy(temp),{counter});
    auto reply=call_reply({count_call("effect-1")});
    reply.message["_responses_output"]=Json::array({{{"type","reasoning"},{"encrypted_content","opaque-ciphertext"}}});
    Provider first; first.replies={reply};
    first.observe=[&](const Json&) {
        const auto id=store.list_tasks()[0]["id"].get<std::string>();
        const auto state=store.checkpoint(id)["state"];
        check(state["phase"]=="model_pending" && state["turns"]==1,"request not checkpointed before model invocation");
    };
    const auto result=run_agent(store,first,broker,{"budget test",1},cancel);
    const auto id=result["task_id"].get<std::string>();
    check(result["status"]=="exhausted" && result["turns"]==1 && counter->executed==1,"first budgeted effect failed");
    Provider second; second.replies={done()};
    rejects([&] { run_agent(store,second,broker,resume_options(id,1),cancel); },"exhausted budget silently reset");
    check(second.calls==0 && store.task(id)["status"]=="exhausted","budget refusal changed task or called model");
    second.observe=[&](const Json& history) {
        check(history.size()==4 && history[2]==reply.message,"native continuation lost across resume");
        check(Json::parse(history[3]["content"].get<std::string>())["output"]["executed"]==1,"prior receipt lost");
    };
    const auto resumed=run_agent(store,second,broker,resume_options(id,2),cancel);
    check(resumed["task_id"]==id && resumed["turns"]==2 && resumed["status"]=="completed" && counter->executed==1,
          "resume duplicated effect or reset identity/budget");
    check(store.checkpoint(id)["state"]["phase"]=="finished","final answer lacks finished checkpoint");
}
void cancellation_between_tools() {
    Temp temp; Store store(temp.root/"state.sqlite3"); std::atomic_bool cancel{false};
    auto counter=std::make_shared<Counter>(); ToolBroker broker(policy(temp),{counter});
    counter->after_effect=[&] {
        const auto id=store.list_tasks()[0]["id"].get<std::string>();
        const auto state=store.checkpoint(id)["state"];
        check(state["phase"]=="tools" && state["next_call"]==0 && state["pending_calls"].size()==2,
              "pending calls were not durable before first effect");
        cancel=true;
    };
    Provider first; first.replies={call_reply({count_call("one"),count_call("two")})};
    const auto result=run_agent(store,first,broker,{"cancel in tool loop"},cancel);
    const auto id=result["task_id"].get<std::string>();
    check(result["status"]=="cancelled" && counter->executed==1,"cancellation did not stop between tools");
    check(store.checkpoint(id)["state"]["next_call"]==1 && store.task(id)["actions"].size()==1,
          "result and checkpoint cursor disagree");
    counter->after_effect={}; cancel=false;
    Provider second; second.replies={done()};
    const auto resumed=run_agent(store,second,broker,resume_options(id),cancel);
    check(resumed["status"]=="completed" && resumed["turns"]==2 && counter->executed==2 && store.task(id)["actions"].size()==2,
          "pending call resume reran a completed effect");
}
struct Seed {
    std::string id;
    Json state,binding;
};
Seed seed_pending(Store& store,const ToolBroker& broker,const std::string& call_id="seed") {
    std::atomic_bool cancel{true}; Provider provider;
    const auto stopped=run_agent(store,provider,broker,{"seeded interruption"},cancel);
    Seed seed; seed.id=stopped["task_id"].get<std::string>();
    const auto envelope=store.checkpoint(seed.id); seed.state=envelope["state"]; seed.binding=envelope["binding"];
    store.resume_task(seed.id,seed.binding);
    const auto call=count_call(call_id);
    seed.state["messages"].push_back(call_reply({call}).message);
    seed.state["phase"]="tools"; seed.state["turns"]=1;
    seed.state["pending_calls"]=Json::array({{{"id",call.id},{"name",call.name},{"arguments",call.arguments}}});
    seed.state["calls_seen"]=Json::array({call.id});
    store.save_checkpoint(seed.id,seed.state,seed.binding);
    return seed;
}
void recovery_receipts() {
    for (const auto mode : {"prepared","completed","denied","unprepared"}) {
        Temp temp; Store store(temp.root/"state.sqlite3"); std::atomic_bool cancel{false};
        auto counter=std::make_shared<Counter>(); ToolBroker broker(policy(temp),{counter});
        const auto seed=seed_pending(store,broker);
        if (std::string(mode)=="prepared") store.prepare_action(seed.id,count_call("seed"));
        if (std::string(mode)=="completed") {
            const auto action=store.prepare_action(seed.id,count_call("seed")); store.dispatch_action(action);
            store.finish_action(action,{true,{{"persisted",true}},"","applied"});
        }
        if (std::string(mode)=="denied") store.deny_action(seed.id,count_call("seed"),"Saved denial");
        store.recover();
        Provider provider; provider.replies={done()};
        provider.observe=[&](const Json& history) {
            check(history.size()==4,"recovered receipt missing from next model request");
            const auto body=Json::parse(history[3]["content"].get<std::string>());
            if (std::string(mode)=="prepared") check(body["ok"]==false && body["effect"]=="none","prepared call did not become synthetic failure");
            if (std::string(mode)=="completed") check(body["output"]["persisted"]==true,"completed receipt was not reused");
            if (std::string(mode)=="denied") check(body["error"]=="Saved denial","saved denial was not reused");
        };
        const auto result=run_agent(store,provider,broker,resume_options(seed.id),cancel);
        const bool failed=std::string(mode)=="prepared" || std::string(mode)=="denied";
        check(result["status"]==(failed ? "needs_attention" : "completed"),"receipt failure flag did not survive resume");
        check(counter->executed==(std::string(mode)=="unprepared" ? 1 : 0),"saved receipt was executed again");
    }
}
void reject_unsafe_resume() {
    for (const auto mode : {"unknown","partial","mismatch","cursor","calls_seen","binding","policy","running"}) {
        Temp temp; Store store(temp.root/"state.sqlite3"); std::atomic_bool cancel{false};
        auto counter=std::make_shared<Counter>(); ToolBroker broker(policy(temp),{counter});
        auto seed=seed_pending(store,broker);
        if (std::string(mode)=="unknown" || std::string(mode)=="partial" || std::string(mode)=="mismatch") {
            auto call=count_call("seed"); if (std::string(mode)=="mismatch") call.arguments={{"different",true}};
            const auto action=store.prepare_action(seed.id,call); store.dispatch_action(action);
            if (std::string(mode)=="partial") store.finish_action(action,{false,{},"partial","partial"});
            if (std::string(mode)=="mismatch") store.finish_action(action,{true,{},"","applied"});
        }
        if (std::string(mode)=="cursor") seed.state["next_call"]=42;
        if (std::string(mode)=="calls_seen") seed.state["calls_seen"]=Json::array();
        store.save_checkpoint(seed.id,seed.state,seed.binding);
        if (std::string(mode)!="running") store.recover();
        Provider provider; provider.replies={done()};
        auto options=resume_options(seed.id);
        if (std::string(mode)=="binding") options.binding={{"switched",true}};
        auto changed_policy=policy(temp);
        if (std::string(mode)=="policy") changed_policy.allow_write=true;
        ToolBroker selected(changed_policy,{counter});
        rejects([&] { run_agent(store,provider,selected,options,cancel); },"unsafe checkpoint resumed");
        check(provider.calls==0 && counter->executed==0,"unsafe checkpoint caused a model call or tool effect");
        check(store.task(seed.id)["status"]==(std::string(mode)=="running" ? "running" : "interrupted"),
              "invalid checkpoint changed task status");
    }
}
void redaction_and_duplicates() {
    Temp temp; Store store(temp.root/"state.sqlite3"); std::atomic_bool cancel{true};
    auto counter=std::make_shared<Counter>(); ToolBroker broker(policy(temp),{counter});
    Provider unused;
    const auto secret=run_agent(store,unused,broker,{"contains very-secret-token",8,4*1024*1024,{"very-secret-token"}},cancel);
    const auto secret_id=secret["task_id"].get<std::string>();
    const auto checkpoint=store.checkpoint(secret_id);
    check(checkpoint["state"]["resumable"]==false && checkpoint.dump().find("very-secret-token")==std::string::npos,
          "redaction did not block unsafe continuation or leaked known key");
    cancel=false;
    rejects([&] { run_agent(store,unused,broker,resume_options(secret_id),cancel); },"redacted history resumed");
    Provider first; first.replies={call_reply({count_call("old-id")})};
    const auto exhausted=run_agent(store,first,broker,{"duplicate across runs",1},cancel);
    Provider duplicate; duplicate.replies={call_reply({count_call("old-id")})};
    const auto failed=run_agent(store,duplicate,broker,resume_options(exhausted["task_id"].get<std::string>(),2),cancel);
    check(failed["status"]=="failed" && counter->executed==1 && failed["turns"]==2,"old call ID was accepted after resume");
}
void failed_model_attempt_budget() {
    Temp temp; Store store(temp.root/"state.sqlite3"); std::atomic_bool cancel{false}; ToolBroker broker(policy(temp));
    Provider broken;
    const auto failed=run_agent(store,broken,broker,{"interrupted model",2},cancel);
    const auto id=failed["task_id"].get<std::string>();
    check(failed["status"]=="failed" && failed["turns"]==1 && store.checkpoint(id)["state"]["phase"]=="model_pending",
          "failed model attempt was not charged durably");
    Provider next; next.replies={done()};
    const auto resumed=run_agent(store,next,broker,resume_options(id,2),cancel);
    check(resumed["status"]=="completed" && resumed["turns"]==2,"resumed model attempt reset total budget");
}
void denial_transaction_failure() {
    Temp temp; const auto database=temp.root/"state.sqlite3";
    Store store(database); std::atomic_bool cancel{false}; ToolBroker broker(policy(temp));
    const auto sql=[&](const char* command) {
        sqlite3* db=nullptr;
        check(sqlite3_open(path_utf8(database).c_str(),&db)==SQLITE_OK,"fault fixture cannot open database");
        const auto result=sqlite3_exec(db,command,nullptr,nullptr,nullptr);
        sqlite3_close(db);
        check(result==SQLITE_OK,"fault fixture SQL failed");
    };
    sql("CREATE TRIGGER fail_denial BEFORE INSERT ON events WHEN NEW.kind='action.denied' "
        "BEGIN SELECT RAISE(ABORT,'Injected denial event failure'); END");
    Provider first; first.replies={call_reply({{"denied-write","write_file",{{"path","blocked.txt"},{"content","x"}}}})};
    const auto failed=run_agent(store,first,broker,{"denial transaction rollback"},cancel);
    const auto id=failed["task_id"].get<std::string>();
    check(failed["status"]=="failed" && store.task(id)["actions"].empty(),"denial fault did not roll back the receipt");
    const auto state=store.checkpoint(id)["state"];
    check(state["phase"]=="tools" && state["next_call"]==0 && state["messages"].size()==3,
          "rolled-back denial advanced checkpoint without a receipt");
    sql("DROP TRIGGER fail_denial");
    Provider next; next.replies={done()};
    const auto resumed=run_agent(store,next,broker,resume_options(id),cancel);
    check(resumed["status"]=="needs_attention" && store.task(id)["actions"].size()==1 &&
          !std::filesystem::exists(temp.root/"blocked.txt"),"denial transaction failure left task unresumable");
}
void proposals_pause_each_call() {
    Temp temp; Store store(temp.root/"state.sqlite3"); std::atomic_bool cancel{false};
    auto proposals=std::make_shared<Proposal>(); auto counter=std::make_shared<Counter>();
    ToolBroker broker(policy(temp),{proposals,counter});
    const ToolCall first_call{"proposal-one","propose_build",Json::object()},
                   second_call{"proposal-two","propose_build",Json::object()};
    Provider first; first.replies={call_reply({first_call,count_call("between"),second_call})};
    const auto waiting=run_agent(store,first,broker,{"pause every proposal",4},cancel);
    const auto id=waiting.at("task_id").get<std::string>();
    check(waiting["status"]=="waiting_approval" && first.calls==1 && proposals->executed==1 && counter->executed==0,
          "proposal did not pause before the next tool");
    check(store.checkpoint(id)["state"]["messages"].size()==3 && store.checkpoint(id)["state"]["next_call"]==0,
          "pending proposal was exposed as a completed tool result");
    Provider next; next.replies={done()};
    rejects([&] { run_agent(store,next,broker,resume_options(id,4),cancel); },"undecided proposal resumed");
    check(next.calls==0 && store.task(id)["status"]=="waiting_approval","undecided resume mutated task");
    const auto action=waiting["approval"]["action_id"].get<std::string>();
    store.decide_build_proposal(id,action,proposal_digest(),false,store.task(id)["workspace"].get<std::string>());
    const auto again=run_agent(store,next,broker,resume_options(id,4),cancel);
    check(again["status"]=="waiting_approval" && next.calls==0 && proposals->executed==2 && counter->executed==1 &&
          again["turns"]==1,"same-turn second proposal skipped approval or spent a model request");
    const auto action2=again["approval"]["action_id"].get<std::string>();
    const auto approved=store.decide_build_proposal(id,action2,proposal_digest(),true,store.task(id)["workspace"].get<std::string>());
    const auto execution=approved["decision"]["execution_action_id"].get<std::string>();
    store.dispatch_action(execution);
    store.finish_action(execution,{false,{{"exit_code",7}},"Compiler failed","none"});
    store.finish_task(approved["decision"]["execution_task_id"].get<std::string>(),"failed","Compiler failed");
    next.observe=[&](const Json& history) {
        check(history.size()==6,"batched proposal history has wrong size");
        const auto rejected=Json::parse(history[3]["content"].get<std::string>());
        const auto failed=Json::parse(history[5]["content"].get<std::string>());
        check(rejected["output"]["decision"]["decision"]=="rejected" && rejected["output"]["execution"].is_null(),
              "rejection not delivered to original tool call");
        check(failed["ok"]==true && failed["output"]["execution"]["result"]["ok"]==false &&
              failed["output"]["execution"]["result"]["output"]["exit_code"]==7,"failed build evidence lost");
    };
    const auto finished=run_agent(store,next,broker,resume_options(id,4),cancel);
    check(finished["status"]=="completed" && finished["turns"]==2 && proposals->executed==2 && counter->executed==1,
          "known failed build prevented repair or replayed tools");
}
void proposal_consumption_failure() {
    Temp temp; const auto database=temp.root/"state.sqlite3";
    Store store(database); std::atomic_bool cancel{false};
    auto proposals=std::make_shared<Proposal>(); ToolBroker broker(policy(temp),{proposals});
    Provider first; first.replies={call_reply({{"proposal","propose_build",Json::object()}})};
    const auto waiting=run_agent(store,first,broker,{"atomic delivery",3},cancel);
    const auto id=waiting["task_id"].get<std::string>();
    store.decide_build_proposal(id,waiting["approval"]["action_id"].get<std::string>(),proposal_digest(),false,
                               store.task(id)["workspace"].get<std::string>());
    const auto sql=[&](const char* command) {
        sqlite3* db=nullptr;
        check(sqlite3_open(path_utf8(database).c_str(),&db)==SQLITE_OK,"proposal fixture cannot open database");
        const auto result=sqlite3_exec(db,command,nullptr,nullptr,nullptr); sqlite3_close(db);
        check(result==SQLITE_OK,"proposal fixture SQL failed");
    };
    sql("CREATE TRIGGER fail_delivery BEFORE INSERT ON events WHEN NEW.kind='build.proposal.consumed' "
        "BEGIN SELECT RAISE(ABORT,'Injected delivery failure'); END");
    Provider next; next.replies={done()};
    const auto failed=run_agent(store,next,broker,resume_options(id,3),cancel);
    const auto state=store.checkpoint(id)["state"];
    check(failed["status"]=="failed" && state["next_call"]==0 && state["messages"].size()==3 && next.calls==0,
          "rolled-back consumption leaked into failure checkpoint or model request");
    sql("DROP TRIGGER fail_delivery");
    const auto completed=run_agent(store,next,broker,resume_options(id,3),cancel);
    check(completed["status"]=="completed" && proposals->executed==1,"delivery recovery repeated proposal");
    int consumed=0;
    for (const auto& event : store.events(id)) if (event["kind"]=="build.proposal.consumed") ++consumed;
    check(consumed==1,"delivery recovery duplicated consumption event");
}
}
void run_resume_kernel_tests() {
#ifdef _WIN32
    proposals_pause_each_call(); proposal_consumption_failure();
#endif
    denial_transaction_failure();
    budget_and_continuation(); cancellation_between_tools(); recovery_receipts();
    reject_unsafe_resume(); redaction_and_duplicates(); failed_model_attempt_budget();
}
