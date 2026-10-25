#include "codestudio/workspace_execution.hpp"
#include "codestudio/build_workspace.hpp"
#include "codestudio/store.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include "../platform/windows_sandbox.hpp"
#elif defined(__linux__)
#include "../platform/linux_sandbox.hpp"
#endif

namespace codestudio {
namespace {
namespace fs = std::filesystem;
void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw std::invalid_argument("Build configuration must contain objects");
    for (const auto& [key, unused] : value.items())
        if (std::ranges::find(allowed, key) == allowed.end()) throw std::invalid_argument("Unknown build field: " + key);
}
std::string string(const Json& value) {
    if (!value.is_string()) throw std::invalid_argument("Build value must be a string");
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > 16384 || result.find('\0') != std::string::npos)
        throw std::invalid_argument("Build string is empty, too long or contains NUL");
    return result;
}
std::uint64_t number(const Json& object, const char* name, std::uint64_t fallback,
                     std::uint64_t low, std::uint64_t high) {
    if (!object.contains(name)) return fallback;
    const auto& value = object[name];
    if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument(std::string("Invalid build limit: ") + name);
    const auto result = value.get<std::uint64_t>();
    if (result < low || result > high) throw std::invalid_argument(std::string("Build limit outside supported range: ") + name);
    return result;
}
fs::path relative_path(const std::string& value) {
    const auto path = utf8_path(value);
    if (path.empty() || path.has_root_path() || value.find(':') != std::string::npos ||
        value.find('\\') != std::string::npos)
        throw std::invalid_argument("Build relative paths must use forward slashes and stay in their declared root");
    for (const auto& part : path)
        if (part == ".." || (part == "." && path != ".")) throw std::invalid_argument("Build relative path traversal is forbidden");
    return path;
}
std::vector<fs::path> paths(const Json& value, bool empty = false) {
    if (!value.is_array() || value.size() > 256 || (!empty && value.empty()))
        throw std::invalid_argument("Build path list must contain 1..256 entries");
    std::vector<fs::path> result;
    for (const auto& entry : value) result.push_back(relative_path(string(entry)));
    return result;
}
// A recipe supplies literal values, never names of ambient host variables.
// Keep system/private-home values under host control and reject common secret
// and proxy names. This name policy is not a detector for secrets in values.
void validate_environment_name(const std::string& name) {
    const auto uppercase = [](char c) { return (c >= 'A' && c <= 'Z') || c == '_'; };
    if (name.empty() || name.size() > 64 || !uppercase(name.front()) ||
        !std::ranges::all_of(name, [&](char c) { return uppercase(c) || (c >= '0' && c <= '9'); }))
        throw std::invalid_argument("Execution environment names must be uppercase identifiers");
    static const std::set<std::string> reserved{
        "APPDATA","LOCALAPPDATA","USERPROFILE","HOME","HOMEDRIVE","HOMEPATH",
        "TEMP","TMP","TMPDIR","SYSTEMROOT","SYSTEMDRIVE","WINDIR","COMSPEC"};
    if (reserved.contains(name) || name.ends_with("_KEY") || name == "KEY" ||
        name.find("TOKEN") != std::string::npos || name.find("SECRET") != std::string::npos ||
        name.find("PASSWORD") != std::string::npos || name.find("CREDENTIAL") != std::string::npos ||
        name.ends_with("_PROXY"))
        throw std::invalid_argument("Execution environment name is reserved: " + name);
}
struct Mount { std::string name; fs::path source; BuildImportOptions options; };
struct Step { std::string executable; std::vector<std::string> arguments; fs::path directory; };
}

struct WorkspaceExecution::Impl {
    fs::path workspace, artifacts;
    std::vector<fs::path> protected_paths, outputs;
    BuildImportOptions sources;
    BuildCopyLimits export_limits;
    std::vector<Mount> mounts;
    std::vector<Step> steps;
    std::map<std::string, std::string> environment;
    std::uint64_t mount_bytes = 8ULL * 1024 * 1024 * 1024;
    std::size_t mount_files = 100000, output_bytes = 65536;
    std::chrono::milliseconds timeout{120000};
    unsigned process_limit = 64, cpu_percent = 80;
    std::uint64_t memory_bytes = 4ULL * 1024 * 1024 * 1024;

    std::string expand(const std::string& input, const std::map<std::string, fs::path>& roots) const {
        std::string result;
        std::size_t cursor = 0;
        for (;;) {
            auto start = input.find("${", cursor);
            if (start == std::string::npos) { result += input.substr(cursor); break; }
            result += input.substr(cursor, start - cursor);
            auto end = input.find('}', start + 2);
            if (end == std::string::npos) throw std::invalid_argument("Unclosed build variable");
            const auto key = input.substr(start + 2, end - start - 2);
            const auto found = roots.find(key);
            if (found == roots.end()) throw std::invalid_argument("Unknown build variable: " + key);
            result += path_utf8(found->second);
            cursor = end + 1;
        }
        return result;
    }
    void validate_templates() const {
        std::map<std::string, fs::path> roots{{"workspace","workspace"},{"home","home"},{"temp","temp"}};
        for (const auto& mount : mounts) roots.emplace(mount.name, mount.name);
        for (const auto& step : steps) {
            if (!step.executable.starts_with("${")) throw std::invalid_argument("Build executable must start with a declared ${root}/ path");
            const auto end = step.executable.find('}');
            if (end == std::string::npos || end + 2 >= step.executable.size() || step.executable[end + 1] != '/')
                throw std::invalid_argument("Invalid build executable template");
            const auto name = step.executable.substr(2, end - 2);
            if (name == "home" || name == "temp") throw std::invalid_argument("Build executable must be in the workspace or a mount");
            (void)relative_path(step.executable.substr(end + 2));
            (void)expand(step.executable, roots);
            for (const auto& argument : step.arguments) (void)expand(argument, roots);
        }
        for (const auto& [key, value] : environment) (void)expand(value, roots);
    }
};

WorkspaceExecution::WorkspaceExecution(const Json& data, const fs::path& workspace,
    const std::vector<fs::path>& protected_paths) : impl_(std::make_unique<Impl>()) {
    auto& plan = *impl_;
    plan.workspace = fs::canonical(workspace);
    plan.protected_paths = protected_paths;
    if (data.dump().size() > 1024 * 1024)
        throw std::invalid_argument("Expanded execution recipe exceeds size limit");
    fields(data, {"version","inputs","exclude","mounts","steps","environment","outputs","artifact_directory","limits"});
    if (!data.contains("version") || !data["version"].is_number_integer() || data["version"] != 2)
        throw std::invalid_argument("Unsupported execution recipe version");
    plan.sources.inputs = paths(data.at("inputs"));
    if (data.contains("exclude")) plan.sources.excludes = paths(data["exclude"], true);
    plan.sources.protected_paths = plan.protected_paths;
    plan.outputs = paths(data.at("outputs"));
    plan.artifacts = relative_path(string(data.at("artifact_directory")));
    if (plan.artifacts == ".") throw std::invalid_argument("Build artifact_directory must be a dedicated subdirectory");
    plan.sources.protected_paths.push_back(plan.workspace / plan.artifacts);
    if (data.contains("mounts")) {
        if (!data["mounts"].is_array() || data["mounts"].size() > 16) throw std::invalid_argument("Build supports up to 16 mounts");
        std::set<std::string> names{"workspace","home","temp"};
        for (const auto& value : data["mounts"]) {
            fields(value, {"name","source","inputs","exclude"});
            Mount mount;
            mount.name = string(value.at("name"));
            if (mount.name.size() > 32 || !std::ranges::all_of(mount.name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }) || !names.insert(mount.name).second)
                throw std::invalid_argument("Build mount names must be unique lowercase identifiers");
            mount.source = utf8_path(string(value.at("source")));
            if (!mount.source.is_absolute()) throw std::invalid_argument("Build mount source must be absolute");
            mount.options.inputs = paths(value.at("inputs"));
            if (value.contains("exclude")) mount.options.excludes = paths(value["exclude"], true);
            mount.options.protected_paths = plan.protected_paths;
            mount.options.protected_paths.push_back(plan.workspace / plan.artifacts);
            plan.mounts.push_back(std::move(mount));
        }
    }
    if (!data.at("steps").is_array() || data["steps"].empty() || data["steps"].size() > 16)
        throw std::invalid_argument("Build must declare 1..16 fixed steps");
    for (const auto& value : data["steps"]) {
        fields(value, {"executable","arguments","cwd"});
        Step step{string(value.at("executable")), {}, relative_path(value.contains("cwd") ? string(value["cwd"]) : ".")};
        if (!value.at("arguments").is_array() || value["arguments"].size() > 256)
            throw std::invalid_argument("Build step must declare a bounded arguments array");
        for (const auto& argument : value["arguments"]) {
            if (argument == "") step.arguments.emplace_back();
            else step.arguments.push_back(string(argument));
        }
        plan.steps.push_back(std::move(step));
    }
    if (data.contains("environment")) {
        const auto& environment = data["environment"];
        if (!environment.is_object() || environment.size() > 64)
            throw std::invalid_argument("Execution environment must contain at most 64 explicit entries");
        for (const auto& [key, value] : environment.items()) {
            validate_environment_name(key);
            plan.environment.emplace(key, string(value));
        }
    }
    const auto limits = data.value("limits", Json::object());
    fields(limits, {"timeout_ms","processes","memory_mb","cpu_percent","log_bytes","source_bytes","source_files","mount_bytes","mount_files","artifact_bytes","artifact_files"});
    plan.timeout = std::chrono::milliseconds(number(limits,"timeout_ms",120000,100,3600000));
    plan.process_limit = static_cast<unsigned>(number(limits,"processes",64,1,256));
    plan.memory_bytes = number(limits,"memory_mb",4096,64,32768) * 1024 * 1024;
    plan.cpu_percent = static_cast<unsigned>(number(limits,"cpu_percent",80,1,100));
    plan.output_bytes = number(limits,"log_bytes",65536,1024,256 * 1024);
    plan.sources.limits.max_bytes = number(limits,"source_bytes",512ULL * 1024 * 1024,1,16ULL * 1024 * 1024 * 1024);
    plan.sources.limits.max_files = number(limits,"source_files",50000,1,200000);
    plan.mount_bytes = number(limits,"mount_bytes",plan.mount_bytes,1,32ULL * 1024 * 1024 * 1024);
    plan.mount_files = number(limits,"mount_files",100000,1,200000);
    plan.export_limits.max_bytes = number(limits,"artifact_bytes",512ULL * 1024 * 1024,1,8ULL * 1024 * 1024 * 1024);
    plan.export_limits.max_files = number(limits,"artifact_files",10000,1,100000);
    plan.validate_templates();
}
WorkspaceExecution::~WorkspaceExecution() = default;

void WorkspaceExecution::validate_available() {
#ifdef __linux__
    LinuxSandbox::validate_available();
#elif !defined(_WIN32)
    throw std::runtime_error("Workspace isolation has no backend for this platform; no fallback is allowed");
#endif
}

ToolResult WorkspaceExecution::execute(const std::atomic_bool& cancelled) const {
    validate_available();
    if (cancelled.load()) return {false, {{"cancelled",true}}, "Build cancelled before staging", "none"};
#if defined(_WIN32) || defined(__linux__)
    const auto& plan = *impl_;
    Json result{{"backend",backend_id},{"steps",Json::array()},
                {"cancelled",false},{"timed_out",false}};
    // One wall deadline covers staging, all process trees and artifact copying.
    // The watchdog only signals cancellation; the process runner reaps the Job
    // and the copy helpers unwind safely before the sandbox can be destroyed.
    std::atomic_bool stop{false}, finished{false}, expired{false};
    const auto deadline = std::chrono::steady_clock::now() + plan.timeout;
    std::jthread watchdog([&](std::stop_token token) {
        while (!token.stop_requested() && !finished.load()) {
            if (cancelled.load()) { stop = true; break; }
            if (std::chrono::steady_clock::now() >= deadline) { expired = true; stop = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    try {
        ProcessRequest initial;
#ifdef _WIN32
        initial.sandbox = SandboxMode::appcontainer;
        WindowsBuildLimits limits{plan.process_limit,static_cast<SIZE_T>(plan.memory_bytes),plan.cpu_percent};
        WindowsSandbox sandbox(initial, &limits);
#else
        initial.sandbox = SandboxMode::linux_namespace;
        LinuxBuildLimits limits{plan.process_limit,plan.memory_bytes,plan.cpu_percent};
        LinuxSandbox sandbox(initial, &limits);
#endif
        const auto source = sandbox.writable_root() / "workspace";
        const auto home = sandbox.writable_root() / "home";
        const auto temp = sandbox.writable_root() / "temp";
        fs::create_directory(source); fs::create_directory(home); fs::create_directory(temp);
        std::map<std::string,fs::path> roots{{"workspace",source},{"home",home},{"temp",temp}};
        const auto imported = import_build_tree(plan.workspace, source, plan.sources, stop);
        std::uint64_t mount_bytes = 0;
        std::size_t mount_files = 0;
        for (const auto& mount : plan.mounts) {
            const auto target = sandbox.readonly_root() / "bin" / mount.name;
            fs::create_directories(target);
            auto options = mount.options;
            options.limits.max_bytes = plan.mount_bytes - mount_bytes;
            options.limits.max_files = plan.mount_files - mount_files;
            const auto copied = import_build_tree(mount.source, target, options, stop);
            mount_bytes += copied.bytes; mount_files += copied.files;
            roots.emplace(mount.name,target);
        }
        result["imported"] = {{"source_files",imported.files},{"source_bytes",imported.bytes},
                              {"mount_files",mount_files},{"mount_bytes",mount_bytes}};
        auto visible_roots = roots;
        for (auto& [name,path] : visible_roots) path = sandbox.build_path(path);
        const auto visible_home = visible_roots.at("home");
        const auto visible_temp = visible_roots.at("temp");
        std::map<std::string,std::string> environment{
#ifdef _WIN32
            {"APPDATA",path_utf8(visible_home)},{"LOCALAPPDATA",path_utf8(visible_home)},
            {"USERPROFILE",path_utf8(visible_home)},{"HOME",path_utf8(visible_home)},
            {"HOMEDRIVE",path_utf8(visible_home.root_name())},{"HOMEPATH","\\" + path_utf8(visible_home.relative_path())},
            {"TEMP",path_utf8(visible_temp)},{"TMP",path_utf8(visible_temp)}};
#else
            {"HOME",path_utf8(visible_home)},{"TMPDIR",path_utf8(visible_temp)},
            {"TEMP",path_utf8(visible_temp)},{"TMP",path_utf8(visible_temp)},
            {"LANG","C.UTF-8"},{"PATH","/tools"}};
#endif
        for (const auto& [key,value] : plan.environment) environment[key] = plan.expand(value,visible_roots);
        std::size_t logged = 0;
        for (std::size_t index = 0; index < plan.steps.size(); ++index) {
            if (stop.load()) throw std::runtime_error("Build interrupted before next step");
            const auto& step = plan.steps[index];
            ProcessRequest request;
            request.executable = utf8_path(plan.expand(step.executable,roots)).lexically_normal();
            request.working_directory = (source / step.directory).lexically_normal();
            request.sandbox = initial.sandbox;
            for (const auto& argument : step.arguments) request.arguments.push_back(plan.expand(argument,visible_roots));
            request.timeout = std::max(std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            request.max_output_bytes = plan.output_bytes - logged;
            sandbox.configure_build(request.executable,request.working_directory,environment);
            const auto step_result = run_build_process(request,stop,sandbox);
            logged += step_result.output.size();
            result["steps"].push_back({{"index",index},{"exit_code",step_result.exit_code},{"output",step_result.output},
                {"timed_out",step_result.timed_out || expired.load()},{"cancelled",cancelled.load()},
                {"output_truncated",step_result.output_truncated}});
            if (step_result.exit_code != 0 || step_result.timed_out || step_result.cancelled || stop.load()) {
                result["timed_out"] = step_result.timed_out || expired.load();
                result["cancelled"] = cancelled.load();
                finished = true;
                return {false,std::move(result),"Isolated build step failed; no artifacts published","none"};
            }
        }
        // Every step returns only after all descendants are reaped. The sandbox
        // still owns its private tree until the host validates/copies outputs.
        if (stop.load()) throw std::runtime_error("Build interrupted before export");
        ToolResult receipt{true,result,{},"applied"};
        receipt.output["artifacts"] = nullptr; // Allocate receipt slots before publication.
        auto& artifact_slot = receipt.output.at("artifacts");
        auto artifacts = export_build_artifacts(source,plan.outputs,plan.workspace,
            plan.artifacts,plan.protected_paths,plan.export_limits,stop);
        artifact_slot.swap(artifacts); // No allocation or throw after publication.
        finished = true;
        return receipt;
    } catch (const std::exception& error) {
        finished = true;
        result["timed_out"] = expired.load();
        result["cancelled"] = cancelled.load();
        return {false,std::move(result),error.what(),"none"};
    }
#else
    return {false,Json::object(),"Workspace isolation backend required","none"};
#endif
}
}
