#include "codestudio/types.hpp"
#include "darwin_launch_state.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("process test: " + message);
}

template <typename Function>
void rejects(Function&& function, const std::string& message) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    check(failed, message);
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

// Restores the caller's environment without printing secret values.
struct ScopedEnvironment {
    std::string name;
    bool present = false;
#ifdef _WIN32
    std::wstring wide_name;
    std::wstring original;
    explicit ScopedEnvironment(const char* key) : name(key), wide_name(name.begin(), name.end()) {
        const auto size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
        present = size > 0;
        if (present) {
            original.resize(size);
            const auto count = GetEnvironmentVariableW(wide_name.c_str(), original.data(), size);
            original.resize(count);
        }
        if (!SetEnvironmentVariableW(wide_name.c_str(), L"process-test-secret")) throw std::runtime_error("test environment setup");
    }
    ~ScopedEnvironment() { SetEnvironmentVariableW(wide_name.c_str(), present ? original.c_str() : nullptr); }
#else
    std::string original;
    explicit ScopedEnvironment(const char* key) : name(key) {
        if (const auto* value = std::getenv(key)) { original = value; present = true; }
        if (setenv(key, "process-test-secret", 1) != 0) throw std::runtime_error("test environment setup");
    }
    ~ScopedEnvironment() {
        if (present) setenv(name.c_str(), original.c_str(), 1);
        else unsetenv(name.c_str());
    }
#endif
};
} // namespace

void run_process_tests(const std::filesystem::path& fixture_path) {
    using namespace std::chrono_literals;
    using codestudio::run_process;
    const auto fixture = std::filesystem::absolute(fixture_path);
    std::atomic_bool cancelled{false};
    codestudio::ProcessRequest request{fixture, {}, fixture.parent_path(), 5s, 65536};

    const std::vector<std::string> arguments{"", "plain", "two words", "a\"b", "trailing\\", "space \\", "\\\"quoted\\\"", "中文参数", "& | ; $(echo ignored)"};
    request.arguments = {"echo"};
    request.arguments.insert(request.arguments.end(), arguments.begin(), arguments.end());
    std::string expected;
    for (const auto& argument : arguments) expected += std::to_string(argument.size()) + ':' + argument + '\n';
    auto result = run_process(request, cancelled);
#ifdef _WIN32
    // Windows CRT stdout text mode emits CRLF.
    std::string normalized;
    for (const auto ch : result.output) if (ch != '\r') normalized += ch;
    result.output = normalized;
#endif
    check(result.exit_code == 0 && result.output == expected, "Unicode/quoting/empty arguments round trip");
    check(!result.timed_out && !result.cancelled && !result.output_truncated, "normal completion flags");
#ifdef __APPLE__
    {
        DarwinLaunchState host_state;
        request.arguments = {"launch-state", std::to_string(host_state.sentinel)};
        result = run_process(request, cancelled);
    }
    check(result.exit_code == 0 && result.output ==
          "descriptor-closed\nstdin-open\nsigpipe-default\nsignal-unblocked\n",
          "Darwin launch closes inherited descriptors, supplies stdin, and resets child signals");
#endif


    request.arguments = {"streams"};
    result = run_process(request, cancelled);
    check(result.output == "stdout-markerstderr-marker", "stdout and stderr share the captured stream");

    request.arguments = {"spam"};
    request.max_output_bytes = 12345;
    result = run_process(request, cancelled);
    check(result.exit_code == 0 && result.output.size() == 12345 && result.output_truncated,
          "large output drains after truncation without deadlock");
    request.max_output_bytes = 0;
    result = run_process(request, cancelled);
    check(result.exit_code == 0 && result.output.empty() && result.output_truncated, "zero output limit");
    request.max_output_bytes = 65536;

    request.arguments = {"exit", "17"};
    result = run_process(request, cancelled);
    check(result.exit_code == 17, "nonzero process exit code");

    request.arguments = {"sleep", "5000"};
    request.timeout = 150ms;
    auto started = std::chrono::steady_clock::now();
    result = run_process(request, cancelled);
    check(result.timed_out && !result.cancelled, "timeout flag");
    check(std::chrono::steady_clock::now() - started < 3s, "timeout is bounded");
    request.timeout = 5s;
    std::jthread cancel_thread([&] { std::this_thread::sleep_for(100ms); cancelled.store(true); });
    result = run_process(request, cancelled);
    cancel_thread.join();
    check(result.cancelled && !result.timed_out, "cancellation flag");
    result = run_process(request, cancelled);
    check(result.cancelled && result.output.empty(), "cancelled request never starts");
    cancelled.store(false);

    request.arguments = {"echo", std::string("bad\0argument", 12)};
    rejects([&] { run_process(request, cancelled); }, "embedded NUL rejected");
    request.arguments = {"echo", std::string("\xc0\xaf", 2)};
    rejects([&] { run_process(request, cancelled); }, "malformed UTF-8 rejected");
    request.arguments = {"echo"};
    const auto saved_executable = request.executable;
    request.executable = fixture.filename();
    rejects([&] { run_process(request, cancelled); }, "relative executable rejected");
    request.executable = fixture.parent_path() / "codestudio-nonexistent-process-fixture";
    rejects([&] { run_process(request, cancelled); }, "missing executable rejected");
    request.executable = saved_executable;

    {
        ScopedEnvironment primary("CODESTUDIO_API_KEY"), fallback("OPENAI_API_KEY");
        request.arguments = {"environment"};
        result = run_process(request, cancelled);
        check(result.exit_code == 0 && result.output.find("present") == std::string::npos &&
              result.output.find("absent") != std::string::npos, "provider API keys excluded from child environment");
    }

    // Only remove the fresh directory this test successfully created; never a shared parent.
    const auto test_directory = std::filesystem::temp_directory_path() /
        ("codestudio-process-test-" + std::to_string(std::random_device{}()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(test_directory), "unique temporary directory");
    struct DirectoryGuard {
        std::filesystem::path path;
        ~DirectoryGuard() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } directory_guard{test_directory};
    const auto marker = test_directory / "descendant marker.txt";

    const auto invalid_program = test_directory / "invalid-program.exe";
    { std::ofstream invalid(invalid_program); invalid << "This is not an executable."; }
    request.executable = invalid_program;
    rejects([&] { run_process(request, cancelled); }, "OS launch failure throws");
    request.executable = fixture;

    request.arguments = {"child", utf8(marker)};
    request.timeout = 250ms;
    result = run_process(request, cancelled);
    check(result.output.find("descendant-started") != std::string::npos, "fixture created descendant");
    check(result.timed_out, "descendant remains supervised after parent exits");
    std::this_thread::sleep_for(1000ms);
    check(!std::filesystem::exists(marker), "timeout terminates descendant before side effect");
}
