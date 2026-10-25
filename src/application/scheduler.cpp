#include "codestudio/scheduler.hpp"
#include "codestudio/scheduler_process.hpp"
#include "codestudio/scheduler_child.hpp"
#include "codestudio/process_observation.hpp"
#include "codestudio/autostart.hpp"
#include "codestudio/cron.hpp"
#include "codestudio/state_reader.hpp"
#include "codestudio/store.hpp"
#include <charconv>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace codestudio {
std::int64_t scheduler_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
namespace {
struct Configuration {
    std::string command,job,owner,expression,timezone;
    std::filesystem::path workspace,database;
    int poll_ms=100,limit=50,count=5,max_restarts=3,restart_delay_ms=1000;
    std::int64_t before=0,after=-1;
    bool once=false,supervise=false;
    std::optional<SchedulerStartFence> expected;
};
std::int64_t integer(const std::string& value,std::int64_t low,std::int64_t high) {
    std::int64_t result=0;
    const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),result);
    if (error!=std::errc{} || end!=value.data()+value.size() || result<low || result>high)
        throw std::runtime_error("Invalid scheduler numeric option");
    return result;
}
Configuration configure(const std::vector<std::string>& argv) {
    Configuration config; config.command=argv.at(1);
    const std::set<std::string> commands{"scheduler-run","scheduler-child","scheduler-supervise","scheduler-start","scheduler-stop","scheduler-status",
        "scheduler-supervisor-status","scheduler-supervisor-events",
        "schedule-list","schedule-show","schedule-cancel","cron-list","cron-show","cron-pause","cron-resume",
        "cron-cancel","cron-preview","scheduler-autostart-enable","scheduler-autostart-status","scheduler-autostart-disable"};
    if (!commands.contains(config.command)) throw std::runtime_error("Unknown scheduler command");
    std::map<std::string,std::string> values;
    for (std::size_t index=2;index<argv.size();++index) {
        const auto& name=argv[index];
        if (name=="--supervise" && (config.command=="scheduler-start" || config.command=="scheduler-autostart-enable")) {
            if (config.supervise) throw std::runtime_error("Repeated --supervise option");
            config.supervise=true; continue;
        }
        if (name=="--once" && config.command=="scheduler-run") {
            if (config.once) throw std::runtime_error("Repeated --once option");
            config.once=true; continue;
        }
        if ((config.command=="schedule-show" || config.command=="schedule-cancel" || config.command=="cron-show" ||
            config.command=="cron-pause" || config.command=="cron-resume" || config.command=="cron-cancel") &&
            !name.starts_with("--") && config.job.empty()) { config.job=name; continue; }
        const bool allowed=name=="--workspace" || name=="--state" ||
            ((config.command=="scheduler-run" || config.command=="scheduler-child" || config.command=="scheduler-supervise" ||
              config.command=="scheduler-start" || config.command=="scheduler-autostart-enable") && name=="--poll-ms") ||
            ((config.command=="scheduler-run" || config.command=="scheduler-child" || config.command=="scheduler-supervise") && name=="--owner") ||
            ((config.command=="scheduler-start" || config.command=="scheduler-supervise" || config.command=="scheduler-autostart-enable") &&
              (name=="--max-restarts" || name=="--restart-delay-ms")) ||
            ((config.command=="scheduler-child" || config.command=="scheduler-supervise") &&
              (name=="--expected-stop-epoch" || name=="--expected-generation")) ||
            ((config.command=="schedule-list" || config.command=="cron-list") && (name=="--before" || name=="--limit")) ||
            (config.command=="scheduler-supervisor-events" && (name=="--after" || name=="--limit")) ||
            (config.command=="cron-preview" && (name=="--cron" || name=="--timezone" || name=="--count" || name=="--after"));
        if (!allowed || index+1>=argv.size() || argv[index+1].starts_with("--"))
            throw std::runtime_error("Unknown scheduler option or missing value: "+name);
        if (!values.emplace(name,argv[++index]).second) throw std::runtime_error("Repeated scheduler option");
    }
    if (config.command=="scheduler-supervise") config.supervise=true;
    if (!config.supervise && (values.contains("--max-restarts") || values.contains("--restart-delay-ms")))
        throw std::runtime_error("Restart options require --supervise or scheduler-supervise");
    if (values.contains("--max-restarts")) config.max_restarts=static_cast<int>(integer(values.at("--max-restarts"),0,100));
    if (values.contains("--restart-delay-ms")) config.restart_delay_ms=static_cast<int>(integer(values.at("--restart-delay-ms"),100,60000));
    const bool epoch=values.contains("--expected-stop-epoch"),generation=values.contains("--expected-generation");
    if (config.command=="scheduler-supervise" && values.contains("--owner")!=epoch)
        throw std::runtime_error("Internal supervisor identity requires both launch fences");
    if (epoch!=generation || (config.command=="scheduler-child" &&
        (!epoch || !values.contains("--owner") || !values.contains("--state") || !values.contains("--poll-ms"))))
        throw std::runtime_error("Internal scheduler child requires all fixed launch options");
    if (epoch) config.expected=SchedulerStartFence{integer(values.at("--expected-stop-epoch"),0,INT64_MAX),
        integer(values.at("--expected-generation"),0,INT64_MAX-1)};
    if (!values.contains("--workspace")) throw std::runtime_error("Scheduler requires an explicit --workspace");
    if ((config.command=="schedule-show" || config.command=="schedule-cancel" || config.command=="cron-show" ||
        config.command=="cron-pause" || config.command=="cron-resume" || config.command=="cron-cancel") && config.job.empty())
        throw std::runtime_error("Schedule command requires a job ID");
    config.workspace=std::filesystem::canonical(utf8_path(values.at("--workspace")));
    if (!std::filesystem::is_directory(config.workspace)) throw std::runtime_error("Scheduler workspace must be a directory");
    config.database=std::filesystem::weakly_canonical(std::filesystem::absolute(values.contains("--state")
        ? utf8_path(values.at("--state")) : config.workspace/".codestudio"/"state.sqlite3"));
    if (values.contains("--poll-ms")) config.poll_ms=static_cast<int>(integer(values.at("--poll-ms"),20,5000));
    if (values.contains("--limit")) config.limit=static_cast<int>(integer(values.at("--limit"),1,100));
    if (values.contains("--before")) config.before=integer(values.at("--before"),0,INT64_MAX);
    if (config.command=="scheduler-supervisor-events")
        config.after=values.contains("--after") ? integer(values.at("--after"),0,INT64_MAX) : 0;
    if (config.command=="cron-preview") {
        if (!values.contains("--cron")) throw std::runtime_error("cron-preview requires --cron");
        config.expression=values.at("--cron");
        config.timezone=cron_timezone(values.contains("--timezone") ? values.at("--timezone") : "local");
        validate_cron(config.expression,config.timezone);
        if (values.contains("--after")) config.after=integer(values.at("--after"),0,INT64_MAX);
        if (values.contains("--count")) config.count=static_cast<int>(integer(values.at("--count"),1,100));
    }
    config.owner=values.contains("--owner") ? values.at("--owner") : new_id();
    if (config.owner.empty() || config.owner.size()>128 || config.owner.find('\0')!=std::string::npos)
        throw std::runtime_error("Invalid scheduler owner ID");
    if (application_redact(config.owner)!=config.owner)
        throw std::runtime_error("Scheduler owner cannot contain the configured credential");
    return config;
}
std::int64_t current_pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}
struct Active {
    std::atomic_bool cancelled{false},done{false};
    std::mutex mutex;
    std::string job;
    Json outcome;
    std::thread thread;
    ~Active() { cancelled.store(true); if (thread.joinable()) thread.join(); }
    std::string current_job() { std::lock_guard guard(mutex); return job; }
};
CommandResult run_worker(const Configuration& config,const std::atomic_bool& shutdown) {
    std::unique_ptr<RunLock> admission_workspace,admission_database;
    if (config.command!="scheduler-child") {
        admission_workspace=std::make_unique<RunLock>(config.workspace/".codestudio"/"supervisor");
        admission_database=std::make_unique<RunLock>(utf8_path(path_utf8(config.database)+".supervisor"));
    }
    // These locks own the daemon, not the task workspace. Idle waiting must
    // leave ordinary runners and trusted approval requests free to execute.
    RunLock workspace_owner(config.workspace/".codestudio"/"scheduler");
    RunLock database_owner(utf8_path(path_utf8(config.database)+".scheduler"));
    Store control(config.database);
    const auto initial=control.scheduler();
    const auto fence=config.expected.value_or(SchedulerStartFence{initial.at("stop_epoch").get<std::int64_t>(),
        initial.at("generation").get<std::int64_t>()});
    if (shutdown.load()) throw std::runtime_error("Scheduler cancelled before registration");
    const auto generation=control.start_scheduler(config.owner,current_pid(),scheduler_now_ms(),fence);
    const auto stop_epoch=fence.stop_epoch;
    std::unique_ptr<Active> active;
    std::int64_t processed=0,next_tick=0,next_heartbeat=0;
    bool stopping=false;
    try {
        for (;;) {
            const auto now=scheduler_now_ms();
            const auto state=control.scheduler();
            if (shutdown.load() || state.at("owner")!=config.owner || state.at("generation")!=generation ||
                state.at("stop_epoch")!=stop_epoch || state.at("state")!="running") stopping=true;
            if (!stopping && now>=next_heartbeat) {
                if (!control.heartbeat_scheduler(config.owner,now)) stopping=true;
                next_heartbeat=now+1000;
            }
            if (active) {
                const auto job_id=active->current_job();
                if (!job_id.empty()) {
                    const auto job=control.schedule(job_id);
                    if (stopping) {
                        control.cancel_schedule(job_id,now,true);
                        active->cancelled.store(true);
                    } else if ((job.at("status")=="claimed" || job.at("status")=="running") &&
                        (job.at("cancel_requested").get<bool>() || now>=job.at("deadline_at_ms").get<std::int64_t>())) {
                        // A scan can advance from an ineligible candidate to
                        // another job while this connection reads the ledger.
                        std::lock_guard guard(active->mutex);
                        if (active->job==job_id) active->cancelled.store(true);
                    }
                }
                if (stopping) active->cancelled.store(true);
                if (active->done.load()) {
                    active->thread.join();
                    const auto result=active->outcome;
                    active.reset();
                    if (!stopping && result.contains("error")) {
                        // A stop may commit while dispatch is checking its
                        // ownership fence. Treat that race as a normal stop.
                        const auto latest=control.scheduler();
                        stopping=shutdown.load() || latest.at("owner")!=config.owner ||
                            latest.at("generation")!=generation || latest.at("stop_epoch")!=stop_epoch ||
                            latest.at("state")!="running";
                        if (!stopping) throw std::runtime_error(result.at("error").get<std::string>());
                    }
                    processed+=result.value("processed",0);
                    if (config.once) break;
                    next_tick=now+config.poll_ms;
                }
            }
            if (stopping && !active) break;
            if (!stopping && !active && now>=next_tick) {
                active=std::make_unique<Active>();
                auto* operation=active.get();
                operation->thread=std::thread([&,operation] {
                    try {
                        operation->outcome={{"processed",0}};
                        const auto candidates=prepare_scheduler_jobs(config.workspace,config.database,config.owner,generation);
                        for (const auto& job : candidates) {
                            if (operation->cancelled.load()) break;
                            if (job.at("binding").at("workspace")!=path_utf8(config.workspace)) continue;
                            const auto id=job.at("id").get<std::string>();
                            { std::lock_guard guard(operation->mutex); operation->job=id; }
                            const auto result=execute_scheduled_job(config.workspace,config.database,id,
                                config.owner,generation,operation->cancelled);
                            operation->outcome=result.value;
                            if (result.value.value("processed",0)>0) break;
                        }
                    } catch (const RunBusy&) { operation->outcome={{"processed",0},{"busy",true}}; }
                    catch (const std::exception& error) {
                        operation->outcome=application_redact({{"error",error.what()}});
                    } catch (...) { operation->outcome={{"error","Unexpected scheduler worker failure"}}; }
                    operation->done.store(true);
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(config.poll_ms));
        }
        active.reset();
        control.finish_scheduler(config.owner);
        return {{{"processed",processed},{"owner",config.owner},{"state","stopped"}},0};
    } catch (...) {
        if (active) {
            active->cancelled.store(true);
            const auto id=active->current_job();
            try { if (!id.empty()) control.cancel_schedule(id,scheduler_now_ms(),true); } catch (...) {}
            active.reset();
        }
        control.finish_scheduler(config.owner);
        throw;
    }
}

CommandResult run_supervisor(const Configuration& config,const std::atomic_bool& shutdown) {
#if !defined(_WIN32) && !defined(__linux__)
    throw std::runtime_error("Worker supervision is unsupported on this platform");
#else
    RunLock admission_workspace(config.workspace/".codestudio"/"supervisor");
    RunLock admission_database(utf8_path(path_utf8(config.database)+".supervisor"));
    Store control(config.database);
    const auto initial=control.scheduler();
    auto fence=config.expected.value_or(SchedulerStartFence{initial.at("stop_epoch").get<std::int64_t>(),
        initial.at("generation").get<std::int64_t>()});
    if (fence.generation==INT64_MAX) throw std::runtime_error("Scheduler generation exhausted");
    const SupervisorRunOptions options{new_id(),config.owner,path_utf8(config.workspace),
        current_process_identity(),current_pid(),config.poll_ms,config.max_restarts,config.restart_delay_ms};
    control.start_supervisor(options,fence,scheduler_now_ms());
    SupervisorRunState record;
    record.phase="starting"; record.generation=fence.generation;
    auto heartbeat_due=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    const auto persist=[&](const std::string& kind) {
        if (!control.update_supervisor(options.id,options.owner,record,scheduler_now_ms(),kind))
            throw std::runtime_error("Supervisor diagnostic ownership changed");
    };
    const auto heartbeat=[&] {
        if (std::chrono::steady_clock::now()>=heartbeat_due) {
            if (!control.heartbeat_supervisor(options.id,options.owner,scheduler_now_ms()))
                throw std::runtime_error("Supervisor diagnostic ownership changed");
            heartbeat_due=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        }
    };
    const auto finish=[&](const std::string& reason) -> CommandResult {
        record.phase="stopped"; record.exit_reason=reason; record.worker_pid=0; record.next_restart_at_ms=0;
        persist("supervisor.stopped");
        return {{{"supervised",true},{"state","stopped"},{"reason",reason},{"restarts",record.restarts},
            {"last_exit_code",record.last_exit_code ? Json(*record.last_exit_code) : Json(nullptr)},
            {"supervisor_pid",current_pid()},{"run_id",options.id}},
            reason=="restart_limit" || reason=="startup_failed" || reason=="internal_error" ? 2 : 0};
    };
    const auto inspect=[&](const Json& state) -> std::string {
        if (shutdown.load()) return "cancelled";
        if (state.at("stop_epoch")!=fence.stop_epoch) return "stop_requested";
        if (state.at("generation")!=fence.generation) return "ownership_changed";
        return {};
    };
    std::int64_t delay=config.restart_delay_ms;
    bool replacement=false;
    try {
        for (;;) {
            if (const auto reason=inspect(control.scheduler()); !reason.empty()) return finish(reason);
            if (fence.generation==INT64_MAX) throw std::runtime_error("Scheduler generation exhausted");
            // Own the exact child and reap before changing the durable phase.
            // Stored PIDs/diagnostics never authorize control or recovery.
            auto child=start_scheduler_child({config.workspace,config.database,config.owner,config.poll_ms,
                fence.stop_epoch,fence.generation});
            if (replacement) ++record.restarts;
            record.phase="starting"; record.worker_pid=child->pid(); record.next_restart_at_ms=0;
            persist("supervisor.starting");
            bool registered=false;
            std::string ending;
            std::optional<std::int64_t> exited;
            const auto register_observed=[&](const Json& state) {
                if (!registered && state.at("generation")==fence.generation+1 &&
                    state.at("owner")==config.owner && state.at("pid")==child->pid()) {
                    ++fence.generation; registered=true; record.generation=fence.generation;
                    if (record.phase!="stopping") record.phase="running";
                    persist("supervisor."+record.phase);
                }
            };
            for (;;) {
                heartbeat();
                const auto state=control.scheduler();
                register_observed(state);
                if (ending.empty()) ending=inspect(state);
                if (!ending.empty() && record.phase!="stopping") {
                    record.phase="stopping"; record.exit_reason=ending;
                    persist("supervisor.stopping");
                }
                if (ending=="cancelled" && state.at("stop_epoch")==fence.stop_epoch)
                    control.stop_scheduler(fence);
                if (const auto status=child->poll_exit()) { exited=*status; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            // Registration may commit between the last observation and exit.
            const auto state=control.scheduler();
            register_observed(state);
            child.reset();
            record.last_exit_code=exited;
            record.worker_pid=0;
            persist("supervisor.child_exited");
            if (ending.empty()) ending=inspect(state);
            if (!ending.empty()) return finish(ending);
            if (!registered) return finish("startup_failed");
            if (record.last_exit_code==0) return finish("normal_exit");
            if (record.restarts>=config.max_restarts) return finish("restart_limit");
            const auto due=std::chrono::steady_clock::now()+std::chrono::milliseconds(delay);
            record.phase="backoff"; record.next_restart_at_ms=scheduler_now_ms()+delay;
            persist("supervisor.backoff");
            while (std::chrono::steady_clock::now()<due) {
                heartbeat();
                if (const auto reason=inspect(control.scheduler()); !reason.empty()) return finish(reason);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (const auto reason=inspect(control.scheduler()); !reason.empty()) return finish(reason);
            replacement=true;
            delay=std::min<std::int64_t>(60000,delay*2);
        }
    } catch (...) {
        // The child has already been reclaimed by its owner. Preserve a bounded
        // reason, never an exception message that could contain credentials.
        record.phase="stopped"; record.exit_reason="internal_error";
        record.worker_pid=0; record.next_restart_at_ms=0;
        try { (void)control.update_supervisor(options.id,options.owner,record,scheduler_now_ms(),"supervisor.stopped"); }
        catch (...) {} // A failed durable write must not mask the original error.
        throw;
    }
#endif
}
}

CommandResult scheduler_command(const std::vector<std::string>& argv,const std::atomic_bool& cancelled) {
    const auto config=configure(argv);
    if (config.command.starts_with("scheduler-autostart-")) {
        if (cancelled.load()) throw std::runtime_error("Autostart control cancelled");
        return {scheduler_autostart(config.workspace,config.database,
            config.command.substr(std::string("scheduler-autostart-").size()),
            AutostartOptions{config.poll_ms,config.supervise,config.max_restarts,config.restart_delay_ms}),0};
    }
    if (config.command=="cron-preview") {
        const auto after=config.after<0 ? scheduler_now_ms() : config.after;
        auto instant=after;
        Json occurrences=Json::array();
        const auto* zone=config.timezone=="UTC" ? nullptr : std::chrono::locate_zone(config.timezone);
        for (int index=0;index<config.count;++index) {
            instant=cron_next(config.expression,config.timezone,instant);
            const std::chrono::sys_time<std::chrono::milliseconds> time{std::chrono::milliseconds(instant)};
            occurrences.push_back({{"at_ms",instant},{"utc",std::format("{:%FT%TZ}",time)},
                {"local",zone ? std::format("{:%F %R %Z}",std::chrono::zoned_time{zone,time}) :
                    std::format("{:%F %R} UTC",time)}});
        }
        return {{{"expression",config.expression},{"timezone",config.timezone},{"after_ms",after},
            {"occurrences",occurrences},{"misfire","coalesce"},{"overlap","skip"}},0};
    }
    StateReader reader(config.database);
    if (config.command=="schedule-list") return {reader.schedules(config.before,config.limit),0};
    if (config.command=="schedule-show") return {reader.schedule(config.job),0};
    if (config.command=="scheduler-status") return {scheduler_status(reader),0};
    if (config.command=="scheduler-supervisor-status") return {supervisor_status(reader),0};
    if (config.command=="scheduler-supervisor-events") return {reader.supervisor_events(config.after,config.limit),0};
    if (config.command=="cron-list") return {reader.crons(config.before,config.limit),0};
    if (config.command=="cron-show") return {reader.cron(config.job),0};
    if (config.command=="scheduler-run" || config.command=="scheduler-child") return run_worker(config,cancelled);
    if (config.command=="scheduler-supervise") return run_supervisor(config,cancelled);
    if (config.command=="schedule-cancel" || config.command=="scheduler-stop" || config.command=="cron-pause" ||
        config.command=="cron-resume" || config.command=="cron-cancel") {
        if (!std::filesystem::is_regular_file(config.database))
            throw std::runtime_error("No state database found for scheduler control");
        Store store(config.database);
        if (config.command.starts_with("cron-"))
            return {store.control_cron(config.job,config.command.substr(5),scheduler_now_ms()),0};
        if (config.command=="schedule-cancel") return {store.cancel_schedule(config.job,scheduler_now_ms()),0};
        auto result=store.stop_scheduler(); result["stop_requested"]=true; return {std::move(result),0};
    }
    if (cancelled.load()) throw std::runtime_error("Scheduler launch cancelled");
    // Fail before launch if the current database cannot be read. A random owner
    // provides the readiness identity; the returned PID is never used to kill.
    const auto initial=reader.scheduler();
    std::vector<std::string> launch{"codestudio-agent",config.supervise ? "scheduler-supervise" : "scheduler-run",
        "--workspace",path_utf8(config.workspace),"--state",path_utf8(config.database),
        "--owner",config.owner,"--poll-ms",std::to_string(config.poll_ms)};
    if (config.supervise) launch.insert(launch.end(),{"--max-restarts",std::to_string(config.max_restarts),
        "--restart-delay-ms",std::to_string(config.restart_delay_ms),
        "--expected-stop-epoch",initial.at("stop_epoch").dump(),"--expected-generation",initial.at("generation").dump()});
    const auto pid=start_scheduler_process(launch);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (std::chrono::steady_clock::now()<deadline) {
        const auto state=reader.scheduler();
        if (state.at("owner")==config.owner && (config.supervise || state.at("pid")==pid) && state.at("state")=="running" &&
            state.at("generation").get<std::int64_t>()>initial.at("generation").get<std::int64_t>()) {
            auto result=state; result["started"]=true;
            if (config.supervise) {
                result["supervised"]=true; result["supervisor_pid"]=pid;
                result["max_restarts"]=config.max_restarts; result["restart_delay_ms"]=config.restart_delay_ms;
            }
            return {std::move(result),0};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("Scheduler startup was not confirmed; inspect scheduler-status before retrying");
}
}
