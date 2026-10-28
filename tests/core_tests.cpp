#include "codestudio/kernel.hpp"
#include <sqlite3.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

void run_tools_tests();
void run_process_tests(const std::filesystem::path& fixture);
void run_responses_tests();
void run_messages_tests();
void run_line_process_tests(const std::filesystem::path& fixture);
void run_mcp_tests(const std::filesystem::path& fixture);
void run_checkpoint_store_tests();
void run_resume_kernel_tests();
void run_sandbox_tests(const std::filesystem::path& fixture);
void run_state_reader_tests();
void run_build_workspace_tests();
void run_workspace_execution_tests();
void run_build_proposal_tests();
void run_build_decision_tests();
void run_scheduler_store_tests();
void run_cron_tests();
void run_cron_store_tests();
void run_autostart_tests();
void run_process_observation_tests();
void run_credentials_tests();

namespace {
using namespace codestudio;
void check(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void rejects(F function, const char* message) {
    bool caught=false; try{function();}catch(const std::exception&){caught=true;}
    check(caught,message);
}
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("codestudio-tests-"+new_id());
    Temp(){std::filesystem::create_directories(root);}
    ~Temp(){std::error_code error;std::filesystem::remove_all(root,error);}
};
class SequenceProvider final : public ModelProvider {
public:
    std::vector<ModelReply> replies;
    std::size_t next=0;
    std::string name() const override {return "test-script";}
    ModelReply complete(const Json&,const Json&,const std::atomic_bool&) override {
        if(next>=replies.size()) throw std::runtime_error("Unexpected model turn");
        return replies[next++];
    }
};
ModelReply tool(const std::string& id, const std::string& name, const Json& arguments) {
    return {"",{{id,name,arguments}},{{"role","assistant"},{"content",nullptr},
        {"tool_calls",Json::array({{{"id",id},{"type","function"},
            {"function",{{"name",name},{"arguments",arguments.dump()}}}}})}},Json::object()};
}
ModelReply final_reply(const std::string& text) {
    return {text,{},{{"role","assistant"},{"content",text}},Json::object()};
}
void persistence_tests() {
    Temp temp; const auto path=temp.root / "state.sqlite3";
    std::string task, prepared, dispatched;
    {
        RunLock lock(path);
        rejects([&]{RunLock other(path);},"concurrent runner was allowed");
        Store store(path);
        task=store.create_task("durable task",path_utf8(temp.root),"fixture");
        prepared=store.prepare_action(task,{"safe","read_file",{{"path","a.txt"}}});
        rejects([&]{store.finish_task(task,"failed","dispatch failed");},"prepared action stranded in terminal task");
        dispatched=store.prepare_action(task,{"uncertain","write_file",{{"path","b.txt"},{"content","x"}}});
        store.dispatch_action(dispatched);
        rejects([&]{store.dispatch_action(dispatched);},"duplicate dispatch accepted");
        rejects([&]{store.finish_task(task,"completed","wrong");},"in-flight action reported completed");
    }
    {
        RunLock lock(path); Store store(path);
        check(store.task(task).at("status")=="running","state was not durable");
        auto recovered=store.recover();
        check(recovered.size()==1 && recovered[0].at("unknown_actions")==1,"recovery missed uncertain action");
        auto row=store.task(task);
        check(row.at("status")=="interrupted","recovery status wrong");
        check(row.at("actions")[0].at("status")=="not_executed","prepared action retried");
        check(row.at("actions")[1].at("effect")=="unknown","unknown effect lost");
        check(store.recover().empty(),"recovery not idempotent");
        rejects([&]{store.finish_action(dispatched,{true,{},"","applied"});},"stale completion accepted after recovery");
        const auto events=store.events(task); check(events.size()>=5,"event history incomplete");
        for(std::size_t i=1;i<events.size();++i)
            check(events[i].at("seq").get<long long>()>events[i-1].at("seq").get<long long>(),"event order broken");
        auto finished=store.create_task("normal",path_utf8(temp.root),"fixture");
        store.finish_task(finished,"completed","done");
        rejects([&]{store.finish_task(finished,"failed","late");},"terminal task mutated");
        rejects([&]{store.prepare_action(finished,{"late","read_file",{{"path","a"}}});},"action added to finished task");
        rejects([&]{store.deny_action(finished,{"late-denial","write_file",{{"path","a"},{"content","x"}}},"denied");},"denied action added to finished task");
        rejects([&]{store.task("missing");},"unknown task accepted");
    }
    sqlite3* db=nullptr;
    check(sqlite3_open(path_utf8(path).c_str(),&db)==SQLITE_OK,"test DB open failed");
    sqlite3_exec(db,"PRAGMA user_version=999",nullptr,nullptr,nullptr);sqlite3_close(db);
    rejects([&]{Store future(path);},"future schema was silently accepted");
}
void redaction_tests() {
    check(redact("prefix-credential",{"prefix","prefix-credential"})=="[REDACTED]",
          "credential prefix exposed the rest of a longer credential");
    check(redact("abcdefgh",{"abcde","defgh"})=="[REDACTED]",
          "overlapping credentials exposed an unmatched suffix");
    check(redact("abcdefgh",{"defgh","abcde"})=="[REDACTED]",
          "redaction depended on credential order");
    check(redact("left:one-two:right",{"one","two"})=="left:[REDACTED]-[REDACTED]:right",
          "redaction changed ordinary text between separate values");
    check(redact("onetwo",{"one","two"})=="[REDACTED][REDACTED]",
          "adjacent credentials were not independently redacted");
    const Json source={{"prefix-credential",Json::array({"abcdefgh",3})}};
    check(redact(source,{"prefix","prefix-credential","abcde","defgh"})==
          Json{{"[REDACTED]",Json::array({"[REDACTED]",3})}},
          "nested credential redaction failed for object keys or arrays");
}
void kernel_tests() {
    redaction_tests();
    Temp temp; Store store(temp.root / "state.sqlite3"); std::atomic_bool cancel{false};
    ToolPolicy policy;policy.workspace=temp.root;policy.allow_write=true;
    policy.protected_paths={temp.root / "state.sqlite3"};
    ToolBroker broker(policy);
    auto demo=make_demo_provider();
    auto result=run_agent(store,*demo,broker,{"offline verification",8,4*1024*1024,{}},cancel);
    check(result.at("status")=="completed","offline demo failed");
    auto row=store.task(result.at("task_id"));
    check(row.at("actions").size()==2,"demo did not persist two actual actions");
    std::ifstream input(temp.root / "CODESTUDIO_DEMO.txt",std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)),{});
    check(content=="Hello from CodeStudio-Agent!\n","demo file differs from intended result");
    policy.allow_write=false; ToolBroker read_only(policy);
    SequenceProvider denied;
    denied.replies={tool("write","write_file",{{"path","denied.txt"},{"content","bad"}}),final_reply("Everything succeeded")};
    result=run_agent(store,denied,read_only,{"write something"},cancel);
    check(result.at("status")=="needs_attention","failed tool became successful task");
    check(!std::filesystem::exists(temp.root / "denied.txt"),"denied write happened");
    SequenceProvider repeated;
    repeated.replies={tool("same","read_file",{{"path","CODESTUDIO_DEMO.txt"}}),
                      tool("same","write_file",{{"path","duplicate.txt"},{"content","bad"}})};
    result=run_agent(store,repeated,broker,{"repeat"},cancel);
    check(result.at("status")=="failed","duplicate model call accepted");
    check(!std::filesystem::exists(temp.root / "duplicate.txt"),"duplicate call executed");
    SequenceProvider budget;
    budget.replies={tool("read","read_file",{{"path","CODESTUDIO_DEMO.txt"}})};
    result=run_agent(store,budget,broker,{"budget",1},cancel);
    check(result.at("status")=="exhausted","model turn budget ignored");
    SequenceProvider secret;
    secret.replies={final_reply("The key is super-secret-value")};
    result=run_agent(store,secret,broker,{"super-secret-value",8,4*1024*1024,{"super-secret-value"}},cancel);
    auto id=result.at("task_id").get<std::string>();
    check(result.dump().find("super-secret-value")==std::string::npos,"secret in returned output");
    check(store.task(id).dump().find("super-secret-value")==std::string::npos,"secret in task storage");
    check(store.events(id).dump().find("super-secret-value")==std::string::npos,"secret in event storage");
    SequenceProvider secret_denied;
    secret_denied.replies={tool("super-secret-value","super-secret-value",Json::object()),final_reply("Denied")};
    result=run_agent(store,secret_denied,broker,{"redact denied tool identity",8,4*1024*1024,{"super-secret-value"}},cancel);
    id=result.at("task_id").get<std::string>();
    check(store.task(id).dump().find("super-secret-value")==std::string::npos,"secret in denied action identity");
    check(store.events(id).dump().find("super-secret-value")==std::string::npos,"secret in denied action event");
    SequenceProvider unused; cancel=true;
    result=run_agent(store,unused,broker,{"cancel before request"},cancel);
    check(result.at("status")=="cancelled" && unused.next==0,"cancellation did not stop model call");
}
}

int main(int argc,char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--redaction-only") {
            redaction_tests();
            std::cout << "PASS overlapping credential redaction\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--cron-only") {
            run_cron_tests(); std::cout << "PASS cron syntax/time zones/DST boundaries\n";
            run_cron_store_tests(); std::cout << "PASS cron atomic occurrences/overlap/recovery/controls\n";
            run_process_observation_tests();std::cout << "PASS read-only process identity observation\n";
        run_autostart_tests(); std::cout << "PASS autostart option validation and native capability boundaries\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--build-io-only") {
            run_build_workspace_tests();
            std::cout << "PASS manifest snapshots and workspace copy capability boundaries\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--state-reader-only") {
            run_state_reader_tests();
            std::cout << "PASS read-only state queries/pagination/commit visibility\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--sandbox-only") {
            const auto directory = std::filesystem::absolute(argv[0]).parent_path();
#ifdef _WIN32
            run_sandbox_tests(directory / "sandbox-fixture.exe");
#else
            run_sandbox_tests(directory / "sandbox-fixture");
#endif
#if defined(__APPLE__)
        std::cout << "PASS unsupported sandbox and package execution refused\n";
#else
        std::cout << "PASS OS sandbox boundaries/stdio/descendants/cleanup\n";
#endif
            return 0;
        }
        persistence_tests();std::cout << "PASS persistence/recovery/transitions/schema\n";
        run_credentials_tests();std::cout << "PASS credential references/validation/environment isolation\n";
        kernel_tests();std::cout << "PASS task loop/policy/budgets/redaction\n";
        run_checkpoint_store_tests();std::cout << "PASS current checkpoint format/atomicity/resume guards\n";
        run_scheduler_store_tests();std::cout << "PASS scheduler persistence/fencing/budgets/atomic recovery\n";
        run_cron_tests();std::cout << "PASS cron syntax/time zones/DST boundaries\n";
        run_cron_store_tests();std::cout << "PASS cron atomic occurrences/overlap/recovery/controls\n";
        run_process_observation_tests();std::cout << "PASS read-only process identity observation\n";
        run_autostart_tests();std::cout << "PASS autostart option validation and native capability boundaries\n";
        run_resume_kernel_tests();std::cout << "PASS durable conversation resume/budgets/no replay\n";
        run_state_reader_tests();std::cout << "PASS read-only state queries/pagination/commit visibility\n";
        run_responses_tests();std::cout << "PASS Responses protocol and continuation\n";
        run_messages_tests();std::cout << "PASS Messages protocol and continuation\n";
        run_tools_tests();std::cout << "PASS workspace tool boundaries\n";
        run_build_workspace_tests();std::cout << "PASS manifest snapshots and workspace copy capability boundaries\n";
        run_workspace_execution_tests();std::cout << "PASS generic execution/manifest compatibility/frozen recipes\n";
        run_build_proposal_tests();std::cout << "PASS build proposals/digests/no execution/receipt validation\n";
        run_build_decision_tests();std::cout << "PASS build decisions/atomic claims/no replay/current schema\n";
        const auto fixture = argc>1 ? std::filesystem::path(argv[1]) :
            std::filesystem::absolute(argv[0]).parent_path() /
#ifdef _WIN32
            "process-fixture.exe";
#else
            "process-fixture";
#endif
        run_process_tests(fixture);std::cout << "PASS process lifecycle/quoting/cancellation\n";
        const auto directory=std::filesystem::absolute(argv[0]).parent_path();
#ifdef _WIN32
        run_line_process_tests(directory / "line-process-fixture.exe");
        std::cout << "PASS persistent process transport/deadlines/environment\n";
        run_mcp_tests(directory / "mcp-fixture.exe");
#else
        run_line_process_tests(directory / "line-process-fixture");
        std::cout << "PASS persistent process transport/deadlines/environment\n";
        run_mcp_tests(directory / "mcp-fixture");
#endif
        std::cout << "PASS MCP grants/lifecycle/schemas/receipts\n";
#ifdef _WIN32
        run_sandbox_tests(directory / "sandbox-fixture.exe");
#else
        run_sandbox_tests(directory / "sandbox-fixture");
#endif
#if defined(__APPLE__)
        std::cout << "PASS unsupported sandbox and package execution refused\n";
#else
        std::cout << "PASS OS sandbox boundaries/stdio/descendants/cleanup\n";
#endif
        return 0;
    } catch(const std::exception& e) {std::cerr << "FAIL: " << e.what() << '\n';return 1;}
}
