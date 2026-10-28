#include "codestudio/autostart.hpp"
#ifdef __APPLE__
#include "../src/platform/autostart_macos.hpp"
#endif

#include <stdexcept>
#include <utility>

namespace {
using namespace codestudio;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("autostart: ") + message);
}
template<class F> void rejects(F&& run, const char* message) {
    bool rejected = false;
    try { run(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
}

void run_autostart_tests() {
#ifdef _WIN32
    using namespace codestudio::autostart_detail;
    const std::filesystem::path executable = LR"(C:\Program Files\CodeStudio-Agent\codestudio-agent.exe)";
    const std::filesystem::path workspace = LR"(D:\Projects\工作 空间\)";
    const std::filesystem::path database = workspace / L"custom state.sqlite3";
    const std::string sid = "S-1-5-21-100-200-300-1001";
    const auto expected = plan(executable, workspace, database, sid, {25});
    check(matches(expected, expected), "own quoted Unicode definition was not recognized");
    check(expected.at("task_name").get<std::string>().starts_with("CodeStudio-Agent-"), "registration missing product prefix");
    check(expected.at("task_name").get<std::string>().size() == 81, "registration identity hash was not retained");
    check(expected.at("uri") == "\\" + expected.at("task_name").get<std::string>(),
        "registration URI does not match the native Task Scheduler registration path");
    check(expected.at("arguments").get<std::string>().find("--poll-ms 25") != std::string::npos, "poll was not frozen");
    check(!expected.contains("api_key") && expected.at("logon_type") == 3 && expected.at("run_level") == 0,
        "registration requested stored credentials or elevation");
    check(expected.at("allow_demand_start") == false && expected.at("trigger_type") == 9,
        "registration could run immediately or outside user logon");

    const auto changed_poll = plan(executable, workspace, database, sid, {100});
    check(expected.at("task_name") == changed_poll.at("task_name"), "poll created another owned task identity");
    check(!matches(expected, changed_poll) && matches(expected, changed_poll, false),
        "registration status/removal cannot recognize its stored poll independently");
    const auto supervised = plan(executable, workspace, database, sid, {25, true, 7, 1500});
    check(supervised.at("task_name") == expected.at("task_name"), "supervision changed registration identity");
    check(supervised.at("arguments").get<std::string>().starts_with("scheduler-supervise "),
        "supervised registration does not launch the supervisor");
    check(supervised.at("arguments").get<std::string>().find("--max-restarts 7") != std::string::npos &&
          supervised.at("arguments").get<std::string>().find("--restart-delay-ms 1500") != std::string::npos,
        "supervision limits were not frozen into the command");
    check(supervised.at("restart_count") == 0 && supervised.at("allow_demand_start") == false,
        "Task Scheduler added implicit restart or immediate execution");
    check(matches(supervised, supervised) && !matches(expected, supervised) && !matches(supervised, expected) &&
          matches(expected, supervised, false) && matches(supervised, expected, false),
        "supervised registration ownership or explicit mode conflict failed");
    for (const auto& options : {AutostartOptions{25, true, 8, 1500}, AutostartOptions{25, true, 7, 2000},
                               AutostartOptions{100, true, 7, 1500}}) {
        const auto altered = plan(executable, workspace, database, sid, options);
        check(!matches(supervised, altered) && matches(supervised, altered, false),
            "stored supervisor limits cannot be inspected independently of requested limits");
    }
    check(plan(executable, L"D:\\Other", database, sid, {25}).at("task_name") != expected.at("task_name"),
        "different workspaces share a registration");
    check(plan(executable, workspace, workspace / L"other.sqlite3", sid, {25}).at("task_name") != expected.at("task_name"),
        "different databases share a registration");
    check(plan(executable, workspace, database, "S-1-5-21-100-200-300-1002", {25}).at("task_name") != expected.at("task_name"),
        "different users share a registration");

    for (const auto* field : {"source", "uri", "principal_id", "group_id", "user_sid", "trigger_user_sid",
                              "trigger_id", "action_id", "action_context", "start_boundary", "end_boundary",
                              "repetition_interval", "repetition_duration"}) {
        auto changed = expected; changed[field] = "unexpected";
        check(!matches(expected, changed, false), "foreign registration metadata or authority accepted");
    }
    for (const auto* field : {"logon_type", "run_level", "trigger_count", "trigger_type", "action_count",
                              "action_type", "multiple_instances", "restart_count"}) {
        auto changed = expected; changed[field] = expected.at(field).get<int>() + 1;
        check(!matches(expected, changed, false), "changed execution authority accepted");
    }
    for (const auto* field : {"trigger_enabled", "zero_delay", "hidden", "allow_demand_start", "disallow_battery",
                              "stop_on_battery", "run_only_idle", "run_only_network", "wake_to_run",
                              "start_when_available", "unlimited_duration", "unlimited_trigger_duration"}) {
        auto changed = expected; changed[field] = !expected.at(field).get<bool>();
        check(!matches(expected, changed, false), "changed lifecycle settings accepted");
    }
    for (const auto* field : {"executable", "working_directory"}) {
        auto changed = expected; changed[field] = "C:\\unrelated";
        check(!matches(expected, changed, false), "foreign executable or directory accepted");
    }
    for (const auto& command : {
        std::string("scheduler-run --workspace D:\\Other --state D:\\Other\\state.sqlite3 --poll-ms 25"),
        expected.at("arguments").get<std::string>() + " --once",
        expected.at("arguments").get<std::string>() + " & unwanted-command",
        std::string("scheduler-run --workspace \"D:\\Projects\" --state \"D:\\state.sqlite3\" --poll-ms 0")}) {
        auto changed = expected; changed["arguments"] = command;
        check(!matches(expected, changed, false), "non-owned command line accepted");
    }
    const auto supervisor_args = supervised.at("arguments").get<std::string>();
    for (const auto& suffix : {" --supervise", " --max-restarts 7", " --restart-delay-ms 1500", " --once",
                              " --provider offline", " & unwanted-command"}) {
        auto altered = supervised; altered["arguments"] = supervisor_args + suffix;
        check(!matches(expected, altered, false), "foreign supervisor arguments accepted");
    }
    for (const auto& replacement : {std::pair{"--max-restarts 7", "--max-restarts -1"},
                                   std::pair{"--max-restarts 7", "--max-restarts 101"},
                                   std::pair{"--max-restarts 7", "--max-restarts 7junk"},
                                   std::pair{"--max-restarts 7", "--max-restarts"},
                                   std::pair{"--max-restarts 7", ""},
                                   std::pair{"--restart-delay-ms 1500", "--restart-delay-ms 99"},
                                   std::pair{"--restart-delay-ms 1500", "--restart-delay-ms 60001"},
                                   std::pair{"--restart-delay-ms 1500", "--restart-delay-ms"},
                                   std::pair{"--restart-delay-ms 1500", ""},
                                   std::pair{"--poll-ms 25", "--poll-ms 19"},
                                   std::pair{"scheduler-supervise", "scheduler-run"}}) {
        auto command = supervisor_args;
        const auto offset = command.find(replacement.first);
        check(offset != std::string::npos, "supervisor command fixture is missing an expected option");
        command.replace(offset, std::string_view(replacement.first).size(), replacement.second);
        auto altered = supervised; altered["arguments"] = command;
        check(!matches(expected, altered, false), "invalid or missing supervisor options accepted");
    }
    auto missing = expected; missing.erase("user_sid");
    check(!matches(expected, missing), "incomplete registration accepted");
    rejects([&] { (void)plan("relative.exe", workspace, database, sid, {25}); }, "relative executable accepted");
    rejects([&] { (void)plan(executable, "relative", database, sid, {25}); }, "relative workspace accepted");
    rejects([&] { (void)plan(executable, workspace, "relative.sqlite3", sid, {25}); }, "relative database accepted");
    rejects([&] { (void)plan(executable, L"D:\\%TEMP%", database, sid, {25}); }, "Task Scheduler path expansion accepted");
    rejects([&] { (void)plan(executable, workspace, database, "domain\\account", {25}); }, "non-SID owner accepted");
    rejects([&] { (void)plan(executable, workspace, database, sid, {19}); }, "small polling interval accepted");
    rejects([&] { (void)plan(executable, workspace, database, sid, {5001}); }, "large polling interval accepted");
#elif defined(__linux__)
    using namespace codestudio::autostart_detail;
    const auto expected=plan("/opt/CodeStudio Agent/agent","/home/user/project $cash %value",
        "/home/user/project $cash %value/state.sqlite3","1000",{25});
    check(matches(expected,expected),"own Linux unit was not recognized");
    const auto unit=expected.at("unit_text").get<std::string>();
    check(unit.find("ExecStart=:\"/opt/CodeStudio Agent/agent\"")!=std::string::npos,
        "systemd environment expansion was not disabled");
    check(unit.find("$cash %%value")!=std::string::npos,"systemd specifier was not escaped");
    check(unit.find("\nWorkingDirectory=/home/user/project $cash %%value\n")!=std::string::npos,
        "WorkingDirectory incorrectly used ExecStart argument quoting");
    const auto literal=plan("/opt/agent","/home/user/quote\"and\\slash%dir","/tmp/state","1000",{25});
    check(literal.at("unit_text").get<std::string>().find("\nWorkingDirectory=/home/user/quote\"and\\slash%%dir\n")!=std::string::npos,
        "WorkingDirectory changed literal quotes or backslashes");
    check(unit.find("WantedBy=default.target")!=std::string::npos && unit.find("Restart=no")!=std::string::npos,
        "Linux unit lifecycle differs from explicit manager-start registration");
    check(unit.find("Environment=")==std::string::npos,"Linux unit persisted an environment");
    auto changed=plan("/opt/CodeStudio Agent/agent","/home/user/project $cash %value",
        "/home/user/project $cash %value/state.sqlite3","1000",{100});
    check(expected.at("task_name")==changed.at("task_name"),"poll changed Linux unit identity");
    check(!matches(expected,changed) && matches(expected,changed,false),"Linux poll conflict matching failed");
    const auto supervised=plan("/opt/CodeStudio Agent/agent","/home/user/project $cash %value",
        "/home/user/project $cash %value/state.sqlite3","1000",{25,true,7,1500});
    check(supervised.at("task_name")==expected.at("task_name"),"supervision changed Linux unit identity");
    check(supervised.at("supervised")==true && supervised.at("max_restarts")==7 && supervised.at("restart_delay_ms")==1500,
        "Linux supervisor options were not retained");
    check(expected.at("supervised")==false && !expected.contains("max_restarts") && !expected.contains("restart_delay_ms"),
        "ordinary Linux unit exposed inactive restart limits");
    const auto supervised_unit=supervised.at("unit_text").get<std::string>();
    check(supervised_unit.find(" scheduler-supervise --workspace ")!=std::string::npos &&
          supervised_unit.find("--max-restarts 7")!=std::string::npos &&
          supervised_unit.find("--restart-delay-ms 1500")!=std::string::npos,
        "Linux unit did not freeze the supervisor command");
    check(supervised_unit.find("\nRestart=no\n")!=std::string::npos,"systemd added a second restart policy");
    check(matches(supervised,supervised) && !matches(expected,supervised) && !matches(supervised,expected) &&
          matches(expected,supervised,false) && matches(supervised,expected,false),"Linux supervision mode matching failed");
    for (const auto& options:{AutostartOptions{25,true,8,1500},AutostartOptions{25,true,7,2000},
                             AutostartOptions{100,true,7,1500}}) {
        const auto altered=plan("/opt/CodeStudio Agent/agent","/home/user/project $cash %value",
            "/home/user/project $cash %value/state.sqlite3","1000",options);
        check(!matches(supervised,altered) && matches(supervised,altered,false),"Linux supervisor limit matching failed");
    }
    for (const auto& replacement:{std::pair{"# Supervised=true\n","# Supervised=false\n"},
                                 std::pair{"# MaxRestarts=7\n","# MaxRestarts=8\n"},
                                 std::pair{"# MaxRestarts=7\n",""},
                                 std::pair{"# RestartDelayMS=1500\n","# RestartDelayMS=99\n"},
                                 std::pair{"# RestartDelayMS=1500\n","# RestartDelayMS=01500\n"},
                                 std::pair{"--max-restarts 7","--max-restarts 101"},
                                 std::pair{"--restart-delay-ms 1500","--restart-delay-ms"},
                                 std::pair{"\nRestart=no\n","\nRestart=always\n"},
                                 std::pair{" scheduler-supervise "," scheduler-run "}}) {
        auto contents=supervised_unit;
        const auto offset=contents.find(replacement.first);
        check(offset!=std::string::npos,"supervisor unit fixture is missing an expected field");
        contents.replace(offset,std::string_view(replacement.first).size(),replacement.second);
        auto altered=supervised; altered["unit_text"]=contents;
        check(!matches(expected,altered,false),"altered Linux supervisor metadata or command accepted");
    }
    for (const auto* key:{"supervised","max_restarts","restart_delay_ms"}) {
        auto altered=supervised; altered.erase(key);
        check(!matches(expected,altered,false),"incomplete Linux supervisor definition accepted");
    }
    for (const auto* key:{"unit_text","task_name","workspace","state_database","user_uid","executable"}) {
        changed=expected; changed[key]="foreign";
        check(!matches(expected,changed,false),"foreign Linux unit accepted");
    }
    check(plan("/opt/agent","/tmp/project","/tmp/state","1001",{25}).at("task_name")!=
        plan("/opt/agent","/tmp/project","/tmp/state","1000",{25}).at("task_name"),"UID not bound to registration");
    rejects([] { (void)plan("relative","/tmp/project","/tmp/state","1000",{25}); },"relative executable accepted");
    rejects([] { (void)plan("/opt/agent","/tmp/../project","/tmp/state","1000",{25}); },"unnormalized workspace accepted");
    rejects([] { (void)plan("/opt/agent","/tmp/project\nExecStart=bad","/tmp/state","1000",{25}); },"unit newline injection accepted");
    rejects([] { (void)plan("/opt/agent","/tmp/project ","/tmp/state","1000",{25}); },"WorkingDirectory trailing space accepted");
    rejects([] { (void)plan("/opt/agent","/tmp/project\\","/tmp/state","1000",{25}); },"WorkingDirectory line continuation accepted");
    rejects([] { (void)plan("/opt/agent","/tmp/project","/tmp/state","root",{25}); },"non-numeric UID accepted");
    rejects([] { (void)plan("/opt/agent","/tmp/project","/tmp/state","1000",{19}); },"invalid Linux poll accepted");
#elif defined(__APPLE__)
    using namespace codestudio::autostart_detail;
    const std::string label="com.codestudio.agent.scheduler.fixture";
    const auto override_text=[&](const std::string& value) { return "\n\tdisabled services = {\n\t\t\""+label+"\" => "+value+"\n\t}\n"; };
    check(macos_launchd_disabled_override(override_text("disabled"),label),"launchd disabled override ignored");
    check(macos_launchd_disabled_override(override_text("true"),label),"launchd boolean disabled override ignored");
    check(!macos_launchd_disabled_override(override_text("enabled"),label) &&
          !macos_launchd_disabled_override(override_text("false"),label),"launchd enabled override rejected");
    check(!macos_launchd_disabled_override(override_text("disabled"),label+"-other"),"launchd label prefix confused with exact identity");
    check(!macos_launchd_disabled_override("disabled services = {\n}",label),"empty launchd override table rejected");
    rejects([&] { (void)macos_launchd_disabled_override("",label); },"missing launchd response accepted");
    rejects([&] { (void)macos_launchd_disabled_override(override_text("unknown"),label); },"unknown launchd state accepted");
    rejects([&] { (void)macos_launchd_disabled_override("disabled services = {\n\""+label+"\" => disabled\n",label); },"truncated launchd response accepted");
    rejects([&] { (void)macos_launchd_disabled_override("disabled services = {\n\""+label+"\" => disabled\n\""+label+"\" => enabled\n}",label); },"duplicate launchd override accepted");
    const auto expected=plan("/Applications/CodeStudio Agent/agent", "/Users/test/project & <tags> \"quotes\" 工作区",
        "/Users/test/state $cash.sqlite3", "501", {25});
    check(matches(expected,expected),"LaunchAgent plan did not match itself");
    const auto text=expected.at("plist_text").get<std::string>();
    check(text.find("&amp; &lt;tags&gt; &quot;quotes&quot;")!=std::string::npos,"LaunchAgent XML escaping failed");
    check(text.find("<key>KeepAlive</key><false/>")!=std::string::npos,"LaunchAgent has automatic restarts");
    check(text.find("<key>RunAtLoad</key><true/>")!=std::string::npos,"LaunchAgent does not run at login");
    check(text.find("<key>LimitLoadToSessionType</key><string>Aqua</string>")!=std::string::npos,"LaunchAgent session is not GUI login");
    check(text.find("EnvironmentVariables")==std::string::npos,"LaunchAgent copies credentials from the environment");
    check(expected.at("arguments").size()==8 && expected.at("arguments")[3]==expected.at("workspace"),"LaunchAgent arguments are not literal");
    const auto supervised=plan("/Applications/CodeStudio Agent/agent", "/Users/test/project & <tags> \"quotes\" 工作区",
        "/Users/test/state $cash.sqlite3", "501", {25,true,7,1500});
    check(expected.at("task_name")==supervised.at("task_name"),"LaunchAgent mode changed registration identity");
    check(matches(supervised,supervised) && !matches(expected,supervised) && !matches(supervised,expected) &&
        matches(expected,supervised,false) && matches(supervised,expected,false),"LaunchAgent mode matching failed");
    check(supervised.at("arguments").size()==12 && supervised.at("arguments")[1]=="scheduler-supervise" &&
        supervised.at("arguments")[9]=="7" && supervised.at("arguments")[11]=="1500","LaunchAgent supervisor limits not frozen");
    for (const auto* key:{"plist_text","task_name","workspace","state_database","user_uid","executable","arguments"}) {
        auto changed=expected; changed[key]="foreign";
        check(!matches(expected,changed,false),"Foreign LaunchAgent contract accepted");
    }
    for (const auto* key:{"supervised","max_restarts","restart_delay_ms"}) {
        auto changed=supervised; changed.erase(key);
        check(!matches(expected,changed,false),"Incomplete LaunchAgent supervisor accepted");
    }
    auto changed=supervised; changed["max_restarts"]=7.0;
    check(!matches(expected,changed,false),"Floating LaunchAgent integer metadata accepted");
    changed=expected; changed["max_restarts"]=3;
    check(!matches(expected,changed,false),"Unsupervised LaunchAgent restart metadata accepted");
    changed=expected; changed["foreign"]=true;
    check(!matches(expected,changed,false),"Extra LaunchAgent contract fields accepted");
    for (const auto& uid:{"root","0501","-1","4294967296","501\n"})
        rejects([&] { (void)plan("/opt/agent","/tmp/project","/tmp/state",uid,{25}); },"Invalid LaunchAgent UID accepted");
    rejects([] { (void)plan("relative","/tmp/project","/tmp/state","501",{25}); },"Relative LaunchAgent executable accepted");
    rejects([] { (void)plan("/opt/agent","/tmp/../project","/tmp/state","501",{25}); },"Unnormalized LaunchAgent workspace accepted");
    rejects([] { (void)plan("/opt/agent","/tmp/project\nInjected","/tmp/state","501",{25}); },"LaunchAgent control character accepted");
    check(plan("/opt/agent","/tmp/project","/tmp/state","502",{25}).at("task_name")!=
        plan("/opt/agent","/tmp/project","/tmp/state","501",{25}).at("task_name"),"LaunchAgent UID not bound to identity");
#else
    rejects([] { (void)scheduler_autostart("/tmp", "/tmp/state", "status"); }, "unsupported platform silently accepted");
#endif
    for (const auto& options : {AutostartOptions{19, true, 3, 1000}, AutostartOptions{5001, true, 3, 1000},
                               AutostartOptions{25, true, -1, 1000}, AutostartOptions{25, true, 101, 1000},
                               AutostartOptions{25, true, 3, 99}, AutostartOptions{25, true, 3, 60001},
                               AutostartOptions{25, false, 7, 1000}, AutostartOptions{25, false, 3, 1500}})
        rejects([&] { validate_autostart_options(options); }, "invalid supervision options accepted");
    validate_autostart_options({20, true, 0, 100});
    validate_autostart_options({5000, true, 100, 60000});
}
