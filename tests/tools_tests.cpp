#include "codestudio/tools.hpp"

#include <fstream>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace codestudio;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("tools: " + message);
}

struct TestDirectory {
    fs::path path;
    TestDirectory() {
        std::random_device random;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            auto candidate = fs::temp_directory_path() / ("codestudio-tools-test-" + std::to_string(random()));
            if (fs::create_directory(candidate)) { path = std::move(candidate); return; }
        }
        throw std::runtime_error("Cannot allocate test directory.");
    }
    ~TestDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

ToolCall read_call(const std::string& path) { return {"read", "read_file", {{"path", path}}}; }
ToolCall write_call(const std::string& path, const std::string& content) {
    return {"write", "write_file", {{"path", path}, {"content", content}}};
}

void denied(const ToolBroker& broker, const ToolCall& call, const std::string& message) {
    bool refused = false;
    try { broker.authorize(call); } catch (const std::exception&) { refused = true; }
    expect(refused, message + " (authorize)");
    const std::atomic_bool cancelled{false};
    const auto result = broker.execute(call, cancelled);
    expect(!result.ok && result.effect == "none" && !result.error.empty(), message + " (execute)");
}

void put_file(const fs::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    output << content;
    expect(output.good(), "fixture file can be written");
}

fs::path process_fixture_path() {
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    expect(size > 0 && size < executable.size(), "current executable path available");
    executable.resize(size);
    return fs::path(executable).parent_path() / "process-fixture.exe";
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string executable(size, '\0');
    expect(_NSGetExecutablePath(executable.data(), &size) == 0, "current executable path available");
    return fs::canonical(fs::path(executable.c_str())).parent_path() / "process-fixture";
#else
    return fs::read_symlink("/proc/self/exe").parent_path() / "process-fixture";
#endif
}
}

void run_tools_tests() {
    TestDirectory root;
    const auto workspace = root.path / "workspace";
    fs::create_directory(workspace);
    put_file(workspace / "initial.txt", "hello");
    put_file(root.path / "outside.txt", "outside");
    fs::create_directory(workspace / ".state");
    put_file(workspace / ".state" / "tasks.db", "private");

    ToolPolicy policy;
    policy.workspace = workspace;
    policy.max_file_bytes = 32;
    policy.protected_paths = {workspace / ".state"};
    ToolBroker readonly(policy);
    const std::atomic_bool cancelled{false};
    const auto read = readonly.execute(read_call("initial.txt"), cancelled);
    expect(read.ok && read.output.at("content") == "hello" && read.effect == "none", "read-only reading succeeds");
    denied(readonly, write_call("new.txt", "no"), "writes need explicit permission");
    denied(readonly, {"unknown", "erase_everything", Json::object()}, "unknown tools rejected");
    denied(readonly, {"process", "run_process", {{"executable", "anything"}, {"arguments", Json::array()}}}, "processes default to denied");
    denied(readonly, {"process", "run_process", {{"executable", "anything"}, {"arguments", Json::array({2})}}}, "process arguments must be strings");
    denied(readonly, {"read", "read_file", {{"path", "initial.txt"}, {"extra", true}}}, "unknown fields rejected");
    denied(readonly, {"read", "read_file", {{"path", 7}}}, "non-string paths rejected");
    denied(readonly, {"read", "read_file", Json::array({"initial.txt"})}, "non-object arguments rejected");
    denied(readonly, {"read", "read_file", Json::object()}, "missing fields rejected");

    policy.allow_write = true;
    ToolBroker writable(policy);
    for (const auto& path : std::vector<std::string>{"", "../outside.txt", "nested/../../outside.txt", "nested\\..\\outside.txt",
             "/absolute.txt", "C:/absolute.txt", "//server/share/a", "initial.txt:stream", "NUL", "con.txt", "COM1.txt", "LPT9", "a. ", ".state/tasks.db"}) {
        denied(writable, write_call(path, "no"), "unsafe write path rejected: " + path);
    }
    denied(writable, write_call(std::string("bad\0name", 8), "no"), "NUL path rejected");
    denied(readonly, read_call("../outside.txt"), "reading outside workspace rejected");
    denied(readonly, read_call(".state/tasks.db"), "protected state cannot be read");
    denied(writable, write_call(".state/new.txt", "no"), "new protected state cannot be written");
    denied(writable, write_call("large.txt", std::string(33, 'x')), "oversized write rejected");
    denied(writable, {"write", "write_file", {{"path", "bad.txt"}, {"content", 1}}}, "non-string content rejected");
    put_file(workspace / "large.txt", std::string(33, 'x'));
    denied(readonly, read_call("large.txt"), "oversized read rejected");
    put_file(workspace / "binary.txt", std::string(1, static_cast<char>(0xff)));
    expect(!readonly.execute(read_call("binary.txt"), cancelled).ok, "non-UTF-8 file content rejected");
    denied(writable, write_call("invalid.txt", std::string(1, static_cast<char>(0xff))), "non-UTF-8 writes rejected");

    const auto first_write = writable.execute(write_call("nested/deeper/new.txt", "first"), cancelled);
    expect(first_write.ok && first_write.effect == "applied" && first_write.output.at("bytes") == 5, "write creates ordinary parent directories");
    const auto replacement = writable.execute(write_call("nested/deeper/new.txt", "replacement"), cancelled);
    expect(replacement.ok && replacement.effect == "applied", "existing file can be atomically replaced: " + replacement.error);
    expect(writable.execute(read_call("nested/deeper/new.txt"), cancelled).output.at("content") == "replacement", "write/read round trip");
    expect(writable.execute(write_call("empty.txt", ""), cancelled).ok, "empty files supported");
    const std::atomic_bool stop{true};
    const auto cancelled_write = writable.execute(write_call("cancelled/new.txt", "no"), stop);
    expect(!cancelled_write.ok && cancelled_write.effect == "none" && !fs::exists(workspace / "cancelled"), "cancelled operation has no effect");
    for (const auto& entry : fs::recursive_directory_iterator(workspace)) {
        expect(!entry.path().filename().string().starts_with(".codestudio-tmp-"), "temporary files cleaned up");
    }

    std::error_code link_error;
    fs::create_symlink(root.path / "outside.txt", workspace / "link.txt", link_error);
    if (!link_error) {
        denied(writable, read_call("link.txt"), "file symlinks cannot be read");
        denied(writable, write_call("link.txt", "no"), "file symlinks cannot be overwritten");
    }
    link_error.clear();
    fs::create_directory_symlink(root.path, workspace / "linked-dir", link_error);
    if (!link_error) {
        denied(writable, read_call("linked-dir/outside.txt"), "directory symlinks cannot be traversed for reading");
        denied(writable, write_call("linked-dir/new.txt", "no"), "directory symlinks cannot be traversed for writing");
        fs::remove(workspace / "linked-dir");
    }
    ToolPolicy file_protected = policy;
    file_protected.protected_paths = {workspace / ".state" / "tasks.db"};
    ToolBroker protect_file(file_protected);
    link_error.clear();
    fs::create_hard_link(workspace / ".state" / "tasks.db", workspace / "state-alias.db", link_error);
    if (!link_error) denied(protect_file, read_call("state-alias.db"), "hard links to explicitly protected files rejected");

    const auto schemas = readonly.schemas();
    expect(schemas.is_array() && schemas.size() == 1 && schemas[0].at("type") == "function", "denied capabilities are omitted from model schemas");
    expect(writable.schemas().size() == 2, "write schema exposed when enabled");
    expect(schemas[0].at("function").at("parameters").at("additionalProperties") == false, "schemas disallow unknown arguments");

    const auto fixture = process_fixture_path();
    expect(fs::is_regular_file(fixture), "process fixture built alongside tests");
    ToolPolicy process_policy = policy;
    process_policy.allowed_executables = {fixture};
    ToolBroker processes(process_policy);
    const auto fixture_u8 = fixture.generic_u8string();
    const std::string executable(reinterpret_cast<const char*>(fixture_u8.data()), fixture_u8.size());
    ToolCall process_call{"process", "run_process", {{"executable", executable}, {"arguments", Json::array({"bytes"})}}};
    const auto binary_output = processes.execute(process_call, cancelled);
    expect(binary_output.ok && binary_output.effect == "applied" && binary_output.output.at("encoding_replaced") == true,
           "successful process applied and invalid output bytes safely replaced");
    expect(!binary_output.output.dump().empty(), "process receipt serializes as valid JSON");
    process_call.arguments["arguments"] = Json::array({"exit", "7"});
    const auto failed_process = processes.execute(process_call, cancelled);
    expect(!failed_process.ok && failed_process.output.at("exit_code") == 7 && failed_process.effect == "unknown", "nonzero process effects remain unknown");
    process_call.arguments["executable"] = "process-fixture";
    denied(processes, process_call, "relative executable paths rejected even when name matches");
}
