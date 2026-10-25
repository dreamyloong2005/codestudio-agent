#include "codestudio/service.hpp"
#include "codestudio/application.hpp"
#include "codestudio/credentials.hpp"
#include "codestudio/state_reader.hpp"
#include "codestudio/store.hpp"
#include "codestudio/scheduler.hpp"
#include "../provider/protocol.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace codestudio {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t input_limit = 1024 * 1024;
constexpr std::size_t output_limit = 4 * 1024 * 1024;
constexpr std::size_t queue_limit = 16 * 1024 * 1024;
constexpr std::int64_t write_timeout_ms = 5000;
constexpr std::size_t operation_history_limit = 64;
constexpr std::size_t operation_result_limit = 256 * 1024;

std::int64_t milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// One reader (the service loop), one writer. No model/SQLite thread can block on
// a slow parent: the bounded queue and write deadline cancel the operation.
class Stdio {
public:
    Stdio() {
#ifdef _WIN32
        input_ = GetStdHandle(STD_INPUT_HANDLE); output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetFileType(input_) != FILE_TYPE_PIPE || GetFileType(output_) != FILE_TYPE_PIPE)
            throw std::runtime_error("serve requires piped stdin and stdout owned by its client");
#else
        if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO))
            throw std::runtime_error("serve requires piped stdin and stdout owned by its client");
        input_flags_ = fcntl(STDIN_FILENO, F_GETFL);
        output_flags_ = fcntl(STDOUT_FILENO, F_GETFL);
        if (input_flags_ < 0 || output_flags_ < 0) throw std::runtime_error("Cannot inspect service pipes");
        if (fcntl(STDIN_FILENO, F_SETFL, input_flags_ | O_NONBLOCK) < 0)
            throw std::runtime_error("Cannot configure service input");
        if (fcntl(STDOUT_FILENO, F_SETFL, output_flags_ | O_NONBLOCK) < 0) {
            fcntl(STDIN_FILENO, F_SETFL, input_flags_);
            throw std::runtime_error("Cannot configure service output");
        }
        std::signal(SIGPIPE, SIG_IGN);
#endif
        writer_ = std::thread([this] { write_loop(); });
    }
    ~Stdio() {
        stop_.store(true); ready_.notify_all();
#ifdef _WIN32
        // Repeated cancellation covers the race between checking stop_ and
        // entering synchronous WriteFile. The native handle stays owned here.
        while (!writer_done_.load()) {
            CancelSynchronousIo(writer_.native_handle());
            std::this_thread::sleep_for(5ms);
        }
#endif
        if (writer_.joinable()) writer_.join();
#ifndef _WIN32
        fcntl(STDIN_FILENO, F_SETFL, input_flags_);
        fcntl(STDOUT_FILENO, F_SETFL, output_flags_);
#endif
    }
    bool send(Json message) {
        auto text = application_redact(std::move(message)).dump(-1, ' ', false, Json::error_handler_t::replace);
        text += '\n';
        std::lock_guard guard(mutex_);
        if (failed_.load() || stop_.load()) return false;
        if (text.size() > output_limit || queued_bytes_ + text.size() > queue_limit) {
            failed_.store(true); return false;
        }
        queued_bytes_ += text.size();
        queue_.push_back(std::move(text)); ready_.notify_one();
        return true;
    }
    // false denotes EOF; lack of currently available input is not EOF.
    bool read(std::string& buffer) {
        char data[65536];
#ifdef _WIN32
        DWORD available = 0;
        if (!PeekNamedPipe(input_, nullptr, 0, nullptr, &available, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) return false;
            throw std::runtime_error("Cannot read service input pipe");
        }
        if (!available) return true;
        DWORD received = 0;
        if (!ReadFile(input_, data, std::min<DWORD>(available, sizeof(data)), &received, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) return false;
            throw std::runtime_error("Cannot read service input");
        }
        buffer.append(data, received); return received != 0;
#else
        const auto received = ::read(STDIN_FILENO, data, sizeof(data));
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
            throw std::runtime_error("Cannot read service input");
        }
        buffer.append(data, static_cast<std::size_t>(received)); return received != 0;
#endif
    }
    bool healthy() {
        const auto started = writing_since_.load();
        if (started && milliseconds() - started > write_timeout_ms) failed_.store(true);
        if (failed_.load()) {
            stop_.store(true); ready_.notify_all();
#ifdef _WIN32
            CancelSynchronousIo(writer_.native_handle());
#endif
            return false;
        }
        return true;
    }
    void fail() noexcept { failed_.store(true); }
    void drain() {
        const auto deadline = Clock::now() + 5s;
        while (healthy() && Clock::now() < deadline) {
            {
                std::lock_guard guard(mutex_);
                if (queue_.empty() && !writing_since_.load()) return;
            }
            std::this_thread::sleep_for(5ms);
        }
        failed_.store(true);
    }
private:
    void write_loop() noexcept {
        try {
            for (;;) {
                std::string line;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock, [&] { return stop_.load() || !queue_.empty(); });
                    if (stop_.load()) break;
                    line = std::move(queue_.front()); queue_.pop_front();
                    writing_since_.store(milliseconds());
                }
                std::size_t offset = 0;
                while (offset < line.size() && !stop_.load()) {
#ifdef _WIN32
                    DWORD sent = 0;
                    if (!WriteFile(output_, line.data() + offset,
                        static_cast<DWORD>(line.size() - offset), &sent, nullptr) || !sent) {
                        failed_.store(true); stop_.store(true); break;
                    }
#else
                    pollfd descriptor{STDOUT_FILENO, POLLOUT, 0};
                    const auto status = poll(&descriptor, 1, 100);
                    if (status < 0 && errno == EINTR) continue;
                    if (status < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                        failed_.store(true); stop_.store(true); break;
                    }
                    if (!status) continue;
                    const auto sent = ::write(STDOUT_FILENO, line.data() + offset, line.size() - offset);
                    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                    if (sent <= 0) { failed_.store(true); stop_.store(true); break; }
#endif
                    offset += static_cast<std::size_t>(sent);
                }
                {
                    std::lock_guard guard(mutex_);
                    queued_bytes_ -= line.size(); writing_since_.store(0);
                }
                if (stop_.load()) break;
            }
        } catch (...) { failed_.store(true); }
        writer_done_.store(true);
    }
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> queue_;
    std::size_t queued_bytes_ = 0;
    std::atomic_bool stop_{false}, failed_{false}, writer_done_{false};
    std::atomic<std::int64_t> writing_since_{0};
#ifdef _WIN32
    HANDLE input_ = nullptr, output_ = nullptr;
#else
    int input_flags_ = -1, output_flags_ = -1;
#endif
};

struct RpcError : std::runtime_error {
    int code;
    std::string kind, recovery, outcome;
    RpcError(int code, const std::string& message, std::string kind = {},
             std::string recovery = "fix_request", std::string outcome = "not_started")
        : std::runtime_error(message), code(code), kind(std::move(kind)),
          recovery(std::move(recovery)), outcome(std::move(outcome)) {}
};
Json error_value(const std::string& session, int code, const std::string& message,
                 std::string kind = {}, std::string recovery = "fix_request",
                 std::string outcome = "not_started") {
    if (kind.empty()) {
        switch (code) {
        case -32700: kind = "parse_error"; break;
        case -32600: kind = "invalid_request"; break;
        case -32601: kind = "method_not_found"; break;
        case -32602: kind = "invalid_params"; break;
        case -32002: kind = "not_initialized"; recovery = "initialize"; break;
        default: kind = "command_failed"; recovery = "query_state"; outcome = "unknown"; break;
        }
    }
    // Redact before bounding: truncating a credential first could expose its prefix.
    auto safe = Json::parse(application_redact(Json(message)).dump(-1,' ',false,
        Json::error_handler_t::replace)).get<std::string>();
    auto end = std::min<std::size_t>(safe.size(),8192);
    if (end < safe.size())
        while (end && (static_cast<unsigned char>(safe[end]) & 0xc0) == 0x80) --end;
    Json result{{"code",code},{"message",safe.substr(0,end)},
        {"data",{{"kind",kind},{"recovery",recovery},{"outcome",outcome},{"session_id",session}}}};
    if (safe.size() > 8192) result["data"]["message_truncated"] = true;
    return result;
}
void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw RpcError(-32602, "Parameters must be an object");
    for (const auto& [key, _] : value.items())
        if (std::ranges::find(allowed, key) == allowed.end()) throw RpcError(-32602, "Unknown parameter: " + key);
}
std::string string_value(const Json& value, const char* key, std::size_t maximum = 65536) {
    if (!value.contains(key) || !value[key].is_string()) throw RpcError(-32602, std::string(key) + " must be a string");
    const auto text = value[key].get<std::string>();
    if (text.empty() || text.size() > maximum || text.find('\0') != std::string::npos)
        throw RpcError(-32602, std::string(key) + " has an invalid length or NUL");
    return text;
}
std::int64_t number(const Json& value, const char* key, std::int64_t fallback) {
    if (!value.contains(key)) return fallback;
    if (!value[key].is_number_integer() ||
        (value[key].is_number_unsigned() && value[key].get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)))
        throw RpcError(-32602, std::string(key) + " must be an integer");
    const auto result = value[key].get<std::int64_t>();
    if (result < 0) throw RpcError(-32602, std::string(key) + " must not be negative");
    return result;
}
bool valid_id(const Json& id) {
    if (id.is_string()) return !id.get_ref<const std::string&>().empty() && id.get_ref<const std::string&>().size() <= 128;
    return id.is_number_integer() && (!id.is_number_unsigned() || id.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT64_MAX));
}
void response(Stdio& io, const Json& id, Json result) {
    io.send({{"jsonrpc","2.0"},{"id",id},{"result",std::move(result)}});
}
void error(Stdio& io, const Json& id, Json value) {
    io.send({{"jsonrpc","2.0"},{"id",id},{"error",std::move(value)}});
}
void notification(Stdio& io, const std::string& method, Json params) {
    if (!io.send({{"jsonrpc","2.0"},{"method",method},{"params",std::move(params)}}))
        throw std::runtime_error("Service output closed or exceeded its budget");
}

struct Configuration { std::filesystem::path workspace, database; };
Configuration configure(const std::vector<std::string>& argv) {
    std::map<std::string,std::string> values;
    for (std::size_t index = 2; index < argv.size(); ++index) {
        const auto key = argv[index];
        if ((key != "--workspace" && key != "--state") || index + 1 == argv.size() ||
            argv[index + 1].starts_with("--")) throw std::runtime_error("serve accepts --workspace and optional --state only");
        if (!values.emplace(key, argv[++index]).second) throw std::runtime_error("Repeated service option");
    }
    if (!values.contains("--workspace")) throw std::runtime_error("serve requires an explicit --workspace");
    Configuration config;
    config.workspace = std::filesystem::canonical(utf8_path(values.at("--workspace")));
    if (!std::filesystem::is_directory(config.workspace)) throw std::runtime_error("Workspace must be a directory");
    config.database = std::filesystem::weakly_canonical(std::filesystem::absolute(values.contains("--state")
        ? utf8_path(values.at("--state")) : config.workspace / ".codestudio" / "state.sqlite3"));
    return config;
}

void append_options(std::vector<std::string>& args, const Json& options, std::string_view mode) {
    const bool resume = mode == "resume";
    fields(options, {"prompt","base_url","model","api","max_turns","timeout_ms","process_timeout_ms",
        "max_output_tokens","stream","allow_write","allow_http_loopback","allow_process","mcp_config","allow_mcp","sandbox","build_config","allow_build_proposals","credential"});
    if (mode == "demo" && !options.empty()) throw RpcError(-32602, "Demo options must be empty; demo grants only its named file write");
    if (mode == "build") {
        fields(options,{"build_config"});
        (void)string_value(options,"build_config");
    }
    if (resume && options.contains("prompt")) throw RpcError(-32602, "Resume cannot replace the task prompt");
    const std::map<std::string,std::string> strings{{"prompt","--prompt"},{"base_url","--base-url"},
        {"model","--model"},{"api","--api"},{"mcp_config","--mcp-config"},{"sandbox","--sandbox"},{"build_config","--build-config"},{"credential","--credential"}};
    for (const auto& [key, flag] : strings) if (options.contains(key)) {
        const auto value = string_value(options,key.c_str());
        if (key == "sandbox" && value != "none" && value != "appcontainer" && value != "linux-namespace")
            throw RpcError(-32602,"sandbox must be none, appcontainer or linux-namespace");
        if (key == "api" && value != "chat-completions" && value != "responses" && value != "messages")
            throw RpcError(-32602,"Unsupported model API");
        if (key == "credential") {
            try { (void)parse_credential_reference(value); }
            catch (const std::exception&) { throw RpcError(-32602,"Invalid credential reference"); }
        }
        args.push_back(flag); args.push_back(value);
    }
    const std::map<std::string,std::string> numbers{{"max_turns","--max-turns"},{"timeout_ms","--timeout-ms"},
        {"process_timeout_ms","--process-timeout-ms"},{"max_output_tokens","--max-output-tokens"}};
    for (const auto& [key, flag] : numbers) if (options.contains(key)) {
        const auto value = number(options,key.c_str(),0);
        const auto low = key.ends_with("timeout_ms") ? 100 : 1;
        const auto high = key.ends_with("timeout_ms") ? 300000 : (key == "max_turns" ? 100 : 1000000);
        if (value < low || value > high) throw RpcError(-32602,key + " is out of range");
        args.push_back(flag); args.push_back(std::to_string(value));
    }
    for (const auto* key : {"stream","allow_write","allow_http_loopback","allow_build_proposals"}) if (options.contains(key)) {
        if (!options[key].is_boolean()) throw RpcError(-32602, std::string(key) + " must be boolean");
        if (std::string_view(key) == "stream") {
            if (resume && options[key].get<bool>()) throw RpcError(-32602,"Omit stream on resume to retain the saved mode; only explicit false is supported");
            if (!options[key].get<bool>()) args.push_back("--no-stream");
        }
        else if (options[key].get<bool>()) {
            args.push_back(std::string_view(key) == "allow_write" ? "--allow-write" :
                (std::string_view(key) == "allow_build_proposals" ? "--allow-build-proposals" : "--allow-http-loopback"));
        }
    }
    for (const auto* key : {"allow_process","allow_mcp"}) if (options.contains(key)) {
        const auto& entries = options[key];
        if (!entries.is_array() || entries.size() > 128) throw RpcError(-32602, std::string(key) + " must be a bounded array");
        for (const auto& entry : entries) {
            const auto value = string_value(Json{{"entry",entry}},"entry",32768);
            args.push_back(std::string_view(key) == "allow_process" ? "--allow-process" : "--allow-mcp"); args.push_back(value);
        }
    }
}

void append_scheduler_launch_options(std::vector<std::string>& args, const Json& params) {
    if (params.contains("poll_ms")) {
        const auto value=number(params,"poll_ms",0);
        if (value<20 || value>5000) throw RpcError(-32602,"poll_ms is out of range");
        args.insert(args.end(),{"--poll-ms",std::to_string(value)});
    }
    if (params.contains("supervise") && !params["supervise"].is_boolean())
        throw RpcError(-32602,"supervise must be boolean");
    const bool supervise=params.value("supervise",false);
    if (!supervise && (params.contains("max_restarts") || params.contains("restart_delay_ms")))
        throw RpcError(-32602,"Restart options require supervise: true");
    if (supervise) args.push_back("--supervise");
    for (const auto* key : {"max_restarts","restart_delay_ms"}) if (params.contains(key)) {
        const auto value=number(params,key,0);
        const bool restarts=std::string_view(key)=="max_restarts";
        if (value<(restarts ? 0 : 100) || value>(restarts ? 100 : 60000))
            throw RpcError(-32602,std::string(key)+" is out of range");
        args.insert(args.end(),{restarts ? "--max-restarts" : "--restart-delay-ms",std::to_string(value)});
    }
}
std::vector<std::string> command(const Configuration& config, const std::string& method, const Json& params) {
    const std::map<std::string,std::string> extensions{
        {"mcp/auth/discover","mcp-auth-discover"},{"mcp/auth/status","mcp-auth-status"},
        {"mcp/auth/login","mcp-auth-login"},{"mcp/auth/logout","mcp-auth-logout"},
        {"plugins/pack","plugin-pack"},{"plugins/install","plugin-install"},
        {"plugins/lock","plugin-lock"},{"plugins/inspect","plugin-inspect"}};
    if (const auto extension=extensions.find(method); extension!=extensions.end()) {
        if (method.starts_with("mcp/auth/")) {
            fields(params,method=="mcp/auth/login" ? std::initializer_list<std::string_view>{"mcp_config","server","login_timeout_ms"} :
                std::initializer_list<std::string_view>{"mcp_config","server"});
        } else if (method=="plugins/pack") fields(params,{"source","output"});
        else if (method=="plugins/install") fields(params,{"bundle","url","sha256","store","allow_http_loopback"});
        else if (method=="plugins/lock") fields(params,{"directory","output"});
        else fields(params,{"lock"});
        std::vector<std::string> args{"codestudio-agent",extension->second};
        for (const auto& [key,value] : params.items()) {
            auto flag="--"+key; std::ranges::replace(flag,'_','-');
            if (key=="login_timeout_ms") {
                const auto timeout=number(params,key.c_str(),0);
                if (timeout<100 || timeout>300000) throw RpcError(-32602,"login_timeout_ms is out of range");
                args.insert(args.end(),{flag,std::to_string(timeout)});
            } else if (key=="allow_http_loopback") {
                if (!value.is_boolean()) throw RpcError(-32602,"allow_http_loopback must be boolean");
                if (value.get<bool>()) args.push_back(flag);
            } else {
                auto text=string_value(params,key.c_str(),key=="server" ? 16 : 32768);
                // Service paths use its fixed workspace, never the launcher's cwd.
                if (key=="mcp_config" || key=="source" || key=="output" || key=="bundle" ||
                    key=="store" || key=="directory" || key=="lock") {
                    auto path=utf8_path(text);
                    if (path.is_relative()) path=config.workspace/path;
                    text=path_utf8(path);
                }
                args.insert(args.end(),{flag,text});
            }
        }
        return args;
    }
    if (method=="crons/create") {
        fields(params,{"name","expression","timezone","options","deadline_ms","max_attempts","retry_delay_ms","max_runs"});
        std::vector<std::string> args{"codestudio-agent","cron-create","--workspace",path_utf8(config.workspace),
            "--state",path_utf8(config.database),"--cron",string_value(params,"expression",256)};
        append_options(args,params.value("options",Json::object()),"run");
        for (const auto* key : {"name","timezone"}) if (params.contains(key)) {
            args.push_back(std::string("--")+key); args.push_back(string_value(params,key,128));
        }
        for (const auto* key : {"deadline_ms","max_attempts","retry_delay_ms","max_runs"}) if (params.contains(key)) {
            const auto value=number(params,key,0);
            const std::string name=key;
            const auto low=name=="max_runs" ? 0 : name=="retry_delay_ms" ? 100 : 1;
            const auto high=name=="max_runs" ? 1000000 : name=="retry_delay_ms" ? 60000 : name=="max_attempts" ? 100 : 604800000;
            if (value<low || value>high) throw RpcError(-32602,name+" is out of range");
            auto flag="--"+name; std::ranges::replace(flag,'_','-');
            args.insert(args.end(),{flag,std::to_string(value)});
        }
        return args;
    }
    if (method=="crons/pause" || method=="crons/resume" || method=="crons/cancel") {
        fields(params,{"cron_id"});
        return {"codestudio-agent","cron-"+method.substr(6),string_value(params,"cron_id",128),
            "--workspace",path_utf8(config.workspace),"--state",path_utf8(config.database)};
    }
    if (method=="crons/preview") {
        fields(params,{"expression","timezone","after","count"});
        std::vector<std::string> args{"codestudio-agent","cron-preview","--workspace",path_utf8(config.workspace),
            "--cron",string_value(params,"expression",256)};
        if (params.contains("timezone")) args.insert(args.end(),{"--timezone",string_value(params,"timezone",128)});
        for (const auto* key : {"after","count"}) if (params.contains(key)) {
            const auto value=number(params,key,0);
            if (std::string_view(key)=="count" && (value<1 || value>100)) throw RpcError(-32602,"count is out of range");
            args.insert(args.end(),{std::string("--")+key,std::to_string(value)});
        }
        return args;
    }
    if (method=="scheduler/autostart/status" || method=="scheduler/autostart/enable" || method=="scheduler/autostart/disable") {
        const bool enable=method=="scheduler/autostart/enable";
        fields(params,enable ? std::initializer_list<std::string_view>{"poll_ms","supervise","max_restarts","restart_delay_ms"} : std::initializer_list<std::string_view>{});
        std::vector<std::string> args{"codestudio-agent","scheduler-autostart-"+method.substr(20),
            "--workspace",path_utf8(config.workspace),"--state",path_utf8(config.database)};
        if (enable) append_scheduler_launch_options(args,params);
        return args;
    }
    if (method == "schedules/create") {
        fields(params,{"task_id","options","delay_ms","deadline_ms","max_attempts","retry_delay_ms"});
        std::vector<std::string> args{"codestudio-agent","schedule-resume",string_value(params,"task_id",128),
            "--workspace",path_utf8(config.workspace),"--state",path_utf8(config.database)};
        append_options(args,params.value("options",Json::object()),"resume");
        const std::map<std::string,std::string> schedule_numbers{
            {"delay_ms","--delay-ms"},{"deadline_ms","--deadline-ms"},
            {"max_attempts","--max-attempts"},{"retry_delay_ms","--retry-delay-ms"}};
        for (const auto& [key, flag] : schedule_numbers) if (params.contains(key)) {
            const auto value = number(params,key.c_str(),0);
            const auto minimum = key == "delay_ms" ? 0 : key == "retry_delay_ms" ? 100 : 1;
            const auto maximum = key == "max_attempts" ? 100 : key == "retry_delay_ms" ? 60000 : 604800000;
            if (value < minimum || value > maximum) throw RpcError(-32602,key + " is out of range");
            args.insert(args.end(),{flag,std::to_string(value)});
        }
        return args;
    }
    if (method == "schedules/cancel" || method == "scheduler/start" || method == "scheduler/stop") {
        const bool cancel = method == "schedules/cancel", start = method == "scheduler/start";
        fields(params,cancel ? std::initializer_list<std::string_view>{"job_id"} :
            start ? std::initializer_list<std::string_view>{"poll_ms","supervise","max_restarts","restart_delay_ms"} : std::initializer_list<std::string_view>{});
        std::vector<std::string> args{"codestudio-agent",cancel ? "schedule-cancel" : start ? "scheduler-start" : "scheduler-stop"};
        if (cancel) args.push_back(string_value(params,"job_id",128));
        args.insert(args.end(),{"--workspace",path_utf8(config.workspace),"--state",path_utf8(config.database)});
        if (start) append_scheduler_launch_options(args,params);
        return args;
    }
    if (method == "builds/review" || method == "builds/approve" || method == "builds/reject") {
        const bool review = method == "builds/review";
        fields(params, review ? std::initializer_list<std::string_view>{"task_id","action_id"} :
                                std::initializer_list<std::string_view>{"task_id","action_id","digest","continue_options"});
        std::vector<std::string> args{"codestudio-agent", review ? "build-review" :
            (method == "builds/approve" ? "build-approve" : "build-reject"),
            string_value(params,"task_id",128),"--action",string_value(params,"action_id",128),
            "--workspace",path_utf8(config.workspace),"--state",path_utf8(config.database)};
        if (!review) {
            const auto digest = string_value(params,"digest",64);
            if (digest.size() != 64 || !std::ranges::all_of(digest, [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                })) throw RpcError(-32602,"digest must be a lowercase SHA256 value");
            args.insert(args.end(), {"--digest",digest});
            if (params.contains("continue_options")) {
                args.push_back("--continue-task");
                append_options(args,params.at("continue_options"),"resume");
            }
        }
        return args;
    }
    const bool resume = method == "tasks/resume";
    fields(params, resume ? std::initializer_list<std::string_view>{"task_id","options"} :
                            std::initializer_list<std::string_view>{"mode","options"});
    const auto mode = resume ? "resume" : string_value(params, "mode", 16);
    if (!resume && mode != "run" && mode != "demo" && mode != "build") throw RpcError(-32602, "mode must be run, demo or build");
    const auto options = params.value("options", Json::object());
    std::vector<std::string> args{"codestudio-agent",mode};
    if (resume) args.push_back(string_value(params,"task_id",128));
    args.insert(args.end(), {"--workspace",path_utf8(config.workspace),"--state",path_utf8(config.database)});
    append_options(args,options,mode);
    return args;
}

struct Operation {
    std::string id, method;
    Json completion;
    std::atomic_bool cancelled{false}, done{false};
    std::thread thread;
    ~Operation() { cancelled.store(true); if (thread.joinable()) thread.join(); }
    Json snapshot(const std::string& session) const {
        // done publishes the immutable completion, including the worker's redaction.
        const bool finished = done.load();
        return {{"session_id",session},{"operation_id",id},{"method",method},
            {"state",finished ? "finished" : "running"},{"cancel_requested",cancelled.load()},
            {"completion",finished ? completion : Json(nullptr)}};
    }
};

int serve(const Configuration& config, const std::atomic_bool& shutdown) {
    Stdio io;
    StateReader reader(config.database);
    const auto session = new_id();
    std::unique_ptr<Operation> operation;
    std::deque<Json> completed_operations;
    std::string last_operation;
    std::string pending;
    bool initialized = false, stopping = false, subscribed = false;
    std::int64_t cursor = 0;
    auto next_poll = Clock::now();
    // io outlives Operation, including every error/unwind path.
    const auto active = [&] { return operation && !operation->done.load(); };
    const auto collect = [&] {
        if (operation && operation->done.load()) {
            completed_operations.push_back(operation->snapshot(session));
            if (completed_operations.size() > operation_history_limit) completed_operations.pop_front();
            last_operation = operation->id;
            operation.reset();
        }
    };
    const auto idle = [&] {
        collect();
        if (active()) throw RpcError(-32000, "An operation is already active", "busy", "wait_for_idle");
    };
    const auto dispatch = [&](const std::string& line) {
        Json id = nullptr;
        try {
            collect();
            Json request;
            try { request = protocol::strict_json(line); }
            catch (...) { throw RpcError(-32700, "Invalid JSON"); }
            if (!request.is_object() || request.value("jsonrpc",Json()) != "2.0" ||
                !request.contains("method") || !request["method"].is_string()) throw RpcError(-32600, "Invalid request");
            // Unsupported notifications never invoke methods with side effects.
            if (!request.contains("id")) return;
            if (!valid_id(request["id"])) throw RpcError(-32600, "Invalid request ID");
            id = request["id"];
            for (const auto& [key, _] : request.items())
                if (key != "jsonrpc" && key != "id" && key != "method" && key != "params") throw RpcError(-32600, "Unknown request field");
            const auto method = string_value(request,"method",128);
            const auto params = request.value("params",Json::object());
            if (!params.is_object()) throw RpcError(-32602, "Parameters must be an object");
            if (method == "initialize") {
                fields(params,{"protocol_version"});
                if (initialized) throw RpcError(-32000, "Service is already initialized", "already_initialized", "none");
                if (number(params,"protocol_version",0) != 2) throw RpcError(-32602, "Unsupported protocol_version; expected 2");
                initialized = true;
                response(io,id,{{"protocol_version",2},{"product","CodeStudio-Agent"},{"version","0.1.0"},{"session_id",session},
                    {"workspace",path_utf8(config.workspace)},{"state_database",path_utf8(config.database)},
                    {"capabilities",{{"task_operations",true},{"event_replay",true},{"text_deltas",true},{"automatic_resume",true},
                        {"background_scheduler",true},{"cron",true},{"operation_lookup",true},{"structured_errors",true},
                        {"mcp_streamable_http",true},{"mcp_oauth",true},{"plugin_locks",true},{"plugin_distribution",true},
#if defined(_WIN32) || defined(__linux__)
                        {"plugin_sandbox",true},
                        {"worker_supervision",true},{"supervised_autostart",true},{"supervisor_diagnostics",true},
                        {"login_autostart",true},
#else
                        {"plugin_sandbox",false},
                        {"worker_supervision",false},{"supervised_autostart",false},{"supervisor_diagnostics",false},
                        {"login_autostart",false},
#endif
                        {"build_proposals",true},{"approval_continuation",true}}},
                    {"limits",{{"input_bytes",input_limit},{"output_bytes",output_limit},{"page_items",100},{"active_operations",1},
                        {"operation_history",operation_history_limit},{"operation_result_bytes",operation_result_limit}}}});
                return;
            }
            if (!initialized) throw RpcError(-32002, "Initialize the service first");
            if (method == "tasks/list" || method == "events/list") {
                const bool tasks = method == "tasks/list";
                fields(params,tasks ? std::initializer_list<std::string_view>{"before","limit"} :
                                      std::initializer_list<std::string_view>{"after","limit"});
                const auto limit = number(params,"limit",50);
                if (limit < 1 || limit > 100) throw RpcError(-32602, "limit must be 1..100");
                response(io,id,tasks ? reader.tasks(number(params,"before",0),static_cast<std::size_t>(limit)) :
                    reader.events(number(params,"after",0),static_cast<std::size_t>(limit))); return;
            }
            if (method == "tasks/get") {
                fields(params,{"task_id"}); response(io,id,reader.task(string_value(params,"task_id",128))); return;
            }
            if (method == "schedules/list") {
                fields(params,{"before","limit"});
                const auto limit = number(params,"limit",50);
                if (limit < 1 || limit > 100) throw RpcError(-32602,"limit must be 1..100");
                response(io,id,reader.schedules(number(params,"before",0),static_cast<std::size_t>(limit))); return;
            }
            if (method == "schedules/get") {
                fields(params,{"job_id"}); response(io,id,reader.schedule(string_value(params,"job_id",128))); return;
            }
            if (method == "scheduler/status") {
                fields(params,{}); response(io,id,scheduler_status(reader)); return;
            }
            if (method == "scheduler/supervisor/status") {
                fields(params,{}); response(io,id,supervisor_status(reader)); return;
            }
            if (method == "scheduler/supervisor/events") {
                fields(params,{"after","limit"});
                const auto after=number(params,"after",0),limit=number(params,"limit",50);
                if (after<0 || limit<1 || limit>100) throw RpcError(-32602,"Invalid supervisor event pagination");
                response(io,id,reader.supervisor_events(after,static_cast<std::size_t>(limit))); return;
            }
            if (method=="crons/list") {
                fields(params,{"before","limit"});
                const auto limit=number(params,"limit",50);
                if (limit<1 || limit>100) throw RpcError(-32602,"limit must be 1..100");
                response(io,id,reader.crons(number(params,"before",0),static_cast<std::size_t>(limit))); return;
            }
            if (method=="crons/get") {
                fields(params,{"cron_id"}); response(io,id,reader.cron(string_value(params,"cron_id",128))); return;
            }
            if (method=="crons/preview" || method=="crons/pause" || method=="crons/resume" || method=="crons/cancel" ||
                method=="scheduler/autostart/status" || method=="scheduler/autostart/enable" || method=="scheduler/autostart/disable") {
                const auto result=execute_command(command(config,method,params),shutdown);
                response(io,id,result.value); return;
            }
            if (method == "schedules/create" || method == "schedules/cancel" ||
                method == "scheduler/start" || method == "scheduler/stop") {
                // Durable scheduler control is independent of this client's
                // foreground Operation and remains available while it runs.
                const auto result = execute_command(command(config,method,params),shutdown);
                response(io,id,result.value); return;
            }
            if (method == "builds/review") {
                const auto result = execute_command(command(config,method,params),shutdown);
                response(io,id,result.value); return;
            }
            if (method == "events/subscribe") {
                fields(params,{"after"}); cursor = number(params,"after",0); subscribed = true;
                next_poll = Clock::now(); response(io,id,{{"after",cursor}}); return;
            }
            if (method == "events/unsubscribe") {
                fields(params,{}); subscribed = false; response(io,id,{{"subscribed",false}}); return;
            }
            if (method == "operations/status") {
                fields(params,{}); response(io,id,{{"session_id",session},{"active",active()},
                    {"operation_id",operation ? operation->id : last_operation}}); return;
            }
            if (method == "operations/get") {
                fields(params,{"session_id","operation_id"});
                const auto expected = string_value(params,"session_id",128);
                const auto target = string_value(params,"operation_id",128);
                if (expected != session) throw RpcError(-32000,"Operation belongs to a different service session",
                    "session_mismatch","query_state","unknown");
                if (operation && operation->id == target) {
                    response(io,id,operation->snapshot(session)); return;
                }
                for (const auto& receipt : completed_operations) if (receipt.at("operation_id") == target) {
                    response(io,id,receipt); return;
                }
                // Absence after eviction/restart never proves that an effect did not happen.
                throw RpcError(-32000,"Operation is unknown or no longer retained in this session",
                    "operation_not_found","query_state","unknown");
            }
            if (method == "operations/cancel") {
                fields(params,{"operation_id"}); const auto target = string_value(params,"operation_id",128);
                if (!active() || operation->id != target) throw RpcError(-32000, "No matching active operation",
                    "operation_not_found","query_operation");
                operation->cancelled.store(true); response(io,id,{{"operation_id",target},{"cancel_requested",true}}); return;
            }
            if (method == "tasks/recover") {
                fields(params,{}); idle();
                const auto result = execute_command({"codestudio-agent","recover","--workspace",path_utf8(config.workspace),
                    "--state",path_utf8(config.database)},shutdown);
                response(io,id,result.value); return;
            }
            if (method == "tasks/start" || method == "tasks/resume" || method == "crons/create" ||
                method == "builds/approve" || method == "builds/reject" ||
                method == "mcp/auth/discover" || method == "mcp/auth/status" || method == "mcp/auth/login" || method == "mcp/auth/logout" ||
                method == "plugins/pack" || method == "plugins/install" || method == "plugins/lock" || method == "plugins/inspect") {
                idle(); auto args = command(config,method,params);
                operation = std::make_unique<Operation>(); operation->id = new_id(); operation->method = method;
                auto* current = operation.get();
                // Queue acceptance first: even an immediate demo failure cannot
                // emit completion before its operation ID is acknowledged.
                response(io,id,{{"session_id",session},{"operation_id",current->id},{"status","accepted"}});
                const auto finish = [&io,current,&session](Json data) {
                    data["operation_id"] = current->id;
                    data["session_id"] = session;
                    data = application_redact(std::move(data));
                    auto retained = data;
                    if (retained.contains("result")) {
                        const auto bytes = retained["result"].dump(-1,' ',false,Json::error_handler_t::replace).size();
                        if (bytes > operation_result_limit) {
                            Json refs = Json::object();
                            for (const auto* key : {"task_id","id"}) {
                                const auto& result = retained["result"];
                                if (result.is_object() && result.contains(key) && result[key].is_string() &&
                                    result[key].get_ref<const std::string&>().size() <= 128)
                                    refs[key] = result[key];
                            }
                            retained["result"] = nullptr;
                            retained["result_omitted"] = true;
                            retained["result_bytes"] = bytes;
                            retained["result_refs"] = std::move(refs);
                        }
                    }
                    current->completion = std::move(retained);
                    current->done.store(true);
                    // Once published the receipt is immutable. A transport allocation
                    // failure must not re-enter finish and race a snapshot reader.
                    try { io.send({{"jsonrpc","2.0"},{"method","operation/finished"},{"params",std::move(data)}}); }
                    catch (...) { io.fail(); }
                };
                try {
                    current->thread = std::thread([args=std::move(args),current,&io,&session,finish] {
                        try {
                            CommandCallbacks callbacks;
                            callbacks.on_text = [&](std::string_view text) {
                                notification(io,"operation/text",{{"operation_id",current->id},{"text",text},{"provisional",true}});
                            };
                            callbacks.on_authorization = [&](const Json& notice) {
                                auto message=notice; message["operation_id"]=current->id;
                                notification(io,"operation/authorization",std::move(message));
                            };
                            const auto result = execute_command(args,current->cancelled,callbacks);
                            finish({{"exit_code",result.exit_code},{"result",result.value}});
                        } catch (const std::exception& e) { finish({{"exit_code",2},{"error",error_value(session,-32000,e.what(),
                            "operation_failed","query_state","unknown")}}); }
                        catch (...) { finish({{"exit_code",2},{"error",error_value(session,-32000,"Unexpected operation failure",
                            "operation_failed","query_state","unknown")}}); }
                    });
                } catch (...) { finish({{"exit_code",2},{"error",error_value(session,-32000,"Cannot create operation worker",
                    "internal_error","query_state","not_started")}}); }
                return;
            }
            if (method == "shutdown") {
                fields(params,{}); stopping = true; response(io,id,{{"stopping",true}}); return;
            }
            throw RpcError(-32601, "Method not found");
        } catch (const RpcError& e) { error(io,id,error_value(session,e.code,e.what(),e.kind,e.recovery,e.outcome)); }
        catch (const std::exception& e) { error(io,id,error_value(session,-32000,e.what())); }
    };
    while (true) {
        if (shutdown.load() || !io.healthy()) stopping = true;
        collect();
        if (!stopping) {
            const bool more = io.read(pending);
            std::size_t processed = 0;
            while (!stopping && processed++ < 32) {
                const auto newline = pending.find('\n');
                if (newline == std::string::npos) break;
                if (newline > input_limit) { error(io,nullptr,error_value(session,-32600,"Input frame exceeds limit")); stopping = true; break; }
                auto line = pending.substr(0,newline); pending.erase(0,newline+1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                dispatch(line);
            }
            if (pending.size() > input_limit) { error(io,nullptr,error_value(session,-32600,"Input buffer exceeds limit")); stopping = true; }
            if (!more) {
                if (!pending.empty()) error(io,nullptr,error_value(session,-32700,"Incomplete JSON frame at EOF"));
                stopping = true;
            }
        }
        if (stopping && operation) operation->cancelled.store(true);
        if (subscribed && io.healthy() && Clock::now() >= next_poll) {
            try {
                auto page = reader.events(cursor,50);
                if (!page.at("items").empty()) {
                    cursor = page.at("next_after").get<std::int64_t>();
                    notification(io,"events/batch",std::move(page));
                }
            } catch (const std::exception& e) {
                subscribed = false;
                const auto failure = error_value(session,-32000,e.what());
                io.send({{"jsonrpc","2.0"},{"method","events/error"},
                    {"params",{{"message",failure.at("message")},{"after",cursor},{"error",failure}}}});
            }
            next_poll = Clock::now() + 50ms;
        }
        if (stopping && !active()) break;
        std::this_thread::sleep_for(5ms);
    }
    operation.reset(); io.drain();
    return io.healthy() ? 0 : 1;
}
}

int serve_stdio(const std::vector<std::string>& argv, const std::atomic_bool& shutdown) {
    try { return serve(configure(argv),shutdown); }
    catch (const std::exception& e) {
        std::cerr << application_redact(Json{{"error",e.what()}}).dump(-1,' ',false,Json::error_handler_t::replace) << '\n';
        return 2;
    }
}
}
