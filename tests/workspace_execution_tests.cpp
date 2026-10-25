#include "codestudio/workspace_execution.hpp"
#include "codestudio/build_manifest.hpp"
#include "codestudio/build_workspace.hpp"
#include "codestudio/store.hpp"
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace codestudio;
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("workspace execution: " + message);
}
template<class F> void rejects(F&& action, const std::string& message) {
    bool denied = false;
    try { action(); } catch (const std::exception&) { denied = true; }
    check(denied, message);
}
struct Temp {
    fs::path root = fs::temp_directory_path() / ("codestudio-execution-tests-" + new_id());
    Temp() {
        check(fs::create_directory(root), "create owned temporary directory");
#ifdef _WIN32
        std::wstring long_name(32768, L'\0');
        const auto count = GetLongPathNameW(root.c_str(), long_name.data(), static_cast<DWORD>(long_name.size()));
        check(count && count < long_name.size(), "expand temporary path");
        long_name.resize(count); root = fs::path(long_name);
#endif
    }
    ~Temp() {
        try {
#if defined(_WIN32) || defined(__linux__)
            safe_remove_owned_tree(root);
#else
            fs::remove(root / "input.txt"); fs::remove(root);
#endif
        } catch (...) {}
    }
};
void write(const fs::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value; output.close();
    check(static_cast<bool>(output), "write source sentinel");
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "read source or published output");
    return {std::istreambuf_iterator<char>(input), {}};
}
Json recipe(const fs::path& tool_directory) {
#ifdef _WIN32
    const std::string fixture="build-fixture.exe";
#else
    const std::string fixture="build-fixture";
#endif
    return {{"version", 2}, {"inputs", Json::array({"input.txt"})},
        {"mounts", Json::array({{{"name", "fixture"}, {"source", path_utf8(tool_directory)},
                                 {"inputs", Json::array({fixture})}}})},
        {"steps", Json::array({{{"executable", "${fixture}/"+fixture},
                                {"arguments", Json::array({"write"})}}})},
        {"outputs", Json::array({"out/payload.txt", "out/identity-0.json"})},
        {"artifact_directory", "artifacts"},
        {"limits", {{"timeout_ms", 10000}, {"processes", 8}, {"memory_mb", 256}}}};
}
}

void run_workspace_execution_tests() {
    using namespace codestudio;
    Temp temp;
    write(temp.root / "input.txt", "immutable input");
    auto tool_directory = temp.root;
#ifdef _WIN32
    std::wstring image(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    check(count && count < image.size(), "locate independent build fixture");
    image.resize(count); tool_directory = fs::path(image).parent_path();
#elif defined(__linux__)
    tool_directory=fs::canonical("/proc/self/exe").parent_path();
#endif
    const auto original = recipe(tool_directory);
    // Validate the public host interface directly: a manifest cannot bypass
    // path, environment, process or export policy.
    auto deny_change = [&](const auto& change, const char* message) {
        auto candidate = original;
        change(candidate);
        rejects([&] { WorkspaceExecution job(candidate, temp.root, {}); }, message);
        check(!fs::exists(temp.root / "artifacts") && !fs::exists(temp.root / "out"),
              "invalid recipe caused filesystem effects");
    };
    deny_change([](Json& value) { value["outputs"] = Json::array({"../outside.txt"}); },
                "host accepted export outside the private workspace");
    deny_change([](Json& value) { value["artifact_directory"] = "../elsewhere"; },
                "host accepted publication outside the granted workspace");
    deny_change([](Json& value) { value["steps"][0]["executable"] = "${ungranted}/tool.exe"; },
                "host accepted an undeclared tool root");
    deny_change([](Json& value) { value["steps"][0]["executable"] = "${fixture}/../tool.exe"; },
                "host accepted executable root traversal");
    deny_change([](Json& value) { value["steps"][0]["executable"] = "${home}/tool.exe"; },
                "host accepted an executable outside declared inputs");
    deny_change([](Json& value) { value["steps"][0]["cwd"] = "../outside"; },
                "host accepted working directory traversal");
    deny_change([](Json& value) { value["limits"]["processes"] = 0; },
                "host accepted an unlimited process tree");
    deny_change([](Json& value) { value["limits"]["memory_mb"] = -1; },
                "host accepted negative memory budget");
    deny_change([](Json& value) { value["network"] = "unrestricted"; },
                "host accepted a manifest-defined network grant");
    for (const auto* name : {"HOME", "home", "SYSTEMROOT", "COMSPEC", "SERVICE_TOKEN", "HTTP_PROXY"}) {
        auto candidate = original;
        candidate["environment"] = {{name, "override"}};
        rejects([&] { WorkspaceExecution job(candidate, temp.root, {}); },
                std::string("host accepted protected environment ") + name);
    }
    auto v2 = original;
    v2["version"] = 2;
    check(normalize_build_manifest(v2) == original, "explicit v2 manifest gained implicit defaults");
    auto explicit_environment = v2;
    explicit_environment["environment"] = {{"BUILD_CACHE", "${home}/cache"}};
    const auto normalized_explicit = normalize_build_manifest(explicit_environment);
    check(normalized_explicit.at("environment") == explicit_environment.at("environment"),
          "generic manifest altered explicit environment or injected tool settings");
    WorkspaceExecution generic_environment(normalized_explicit, temp.root, {});
    auto incomplete = v2;
    incomplete.erase("steps");
    rejects([&] { (void)normalize_build_manifest(incomplete); }, "missing explicit steps accepted");
    auto unknown_field = v2;
    unknown_field["unexpected"] = Json::object();
    rejects([&] { (void)normalize_build_manifest(unknown_field); }, "unknown manifest field accepted");
    for (const auto& version : {Json(true), Json(0), Json(1), Json(3), Json("2")}) {
        auto invalid = v2;
        invalid["version"] = version;
        rejects([&] { (void)normalize_build_manifest(invalid); }, "invalid manifest version accepted");
        rejects([&] { WorkspaceExecution job(invalid, temp.root, {}); }, "invalid recipe version accepted");
    }
    v2["environment"] = {{"HOME", "${workspace}"}};
    const auto normalized = normalize_build_manifest(v2);
    rejects([&] { WorkspaceExecution job(normalized, temp.root, {}); },
            "manifest normalization bypassed host environment validation");

#if defined(_WIN32) || defined(__linux__)
#ifdef __linux__
    if (!std::getenv("CODESTUDIO_CGROUP_ROOT")) return;
#endif
    // No tool broker, provider, manifest file or XMake is involved. The service
    // must own its validated input even when the caller discards its JSON.
    auto supplied = original;
    WorkspaceExecution frozen(supplied, temp.root, {});
    supplied = nullptr;
    std::atomic_bool cancelled{true};
    const auto stopped = frozen.execute(cancelled);
    check(!stopped.ok && stopped.effect == "none" && stopped.output.at("cancelled") == true,
          "cancelled job did not stop before import");
    check(!fs::exists(temp.root / "artifacts"), "cancelled job published artifacts");
    cancelled = false;
    const auto result = frozen.execute(cancelled);
    check(result.ok && result.effect == "applied", "direct immutable execution failed: " + result.error);
    const auto bundle = utf8_path(result.output.at("artifacts").at("directory").get<std::string>());
    check(read(bundle / "files/out/payload.txt") == "immutable input\nprivate build output\n",
          "published result does not match the frozen recipe");
    const auto identity = Json::parse(read(bundle / "files/out/identity-0.json"));
#ifdef _WIN32
    check(identity.at("lpac") == true, "direct service bypassed OS isolation");
#else
    check(identity.at("linux_namespace") == true && identity.at("seccomp_mode")==2,
          "direct service bypassed Linux kernel isolation");
#endif
    const auto& environment = identity.at("build_environment");
    check(environment.at("XMAKE_GLOBALDIR").is_null() && environment.at("XMAKE_PKG_INSTALLDIR").is_null() &&
          environment.at("XMAKE_PROGRAM_DIR").is_null(), "generic service injected XMake settings");
    check(read(temp.root / "input.txt") == "immutable input" && !fs::exists(temp.root / "out"),
          "direct service modified the original source workspace");
#endif
}
