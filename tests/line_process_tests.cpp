#include "codestudio/line_process.hpp"

#include <cstdlib>
#include <fstream>
#include <random>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("line process test: ") + message);
}

template <typename Function>
void rejects(Function&& function, const char* message, std::string_view expected = {}) {
    try { function(); }
    catch (const std::exception& error) {
        check(expected.empty() || std::string_view(error.what()).find(expected) != std::string_view::npos, message);
        return;
    }
    check(false, message);
}

struct ScopedEnvironment {
#ifdef _WIN32
    std::wstring name, original;
    bool present;
    explicit ScopedEnvironment(const char* key) : name(key, key + std::char_traits<char>::length(key)) {
        const auto size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        present = size > 0;
        if (present) {
            original.resize(size);
            original.resize(GetEnvironmentVariableW(name.c_str(), original.data(), size));
        }
        check(SetEnvironmentVariableW(name.c_str(), L"test-secret-do-not-forward"), "set test environment");
    }
    ~ScopedEnvironment() { SetEnvironmentVariableW(name.c_str(), present ? original.c_str() : nullptr); }
#else
    std::string name, original;
    bool present = false;
    explicit ScopedEnvironment(const char* key) : name(key) {
        if (const auto* value = std::getenv(key)) { original = value; present = true; }
        check(setenv(key, "test-secret-do-not-forward", 1) == 0, "set test environment");
    }
    ~ScopedEnvironment() { if (present) setenv(name.c_str(), original.c_str(), 1); else unsetenv(name.c_str()); }
#endif
};
} // namespace

void run_line_process_tests(const std::filesystem::path& fixture_path) {
    using codestudio::LineProcess;
    const auto fixture = std::filesystem::absolute(fixture_path);
    codestudio::ProcessRequest request{fixture, {"echo"}, fixture.parent_path(), 5s, 1024 * 1024};
    std::atomic_bool cancelled{false};
    {
        LineProcess child(request);
        for (const auto* line : {"{\"id\":1}", "中文 UTF-8", "", "separate request"}) {
            child.send(line, Clock::now() + 3s, cancelled);
            check(child.receive(Clock::now() + 3s, cancelled) == line, "persistent UTF-8 request/response round trip");
        }
        const std::string large(128 * 1024, 'a');
        child.send(large, Clock::now() + 3s, cancelled);
        check(child.receive(Clock::now() + 3s, cancelled) == large, "large request streams through finite pipe buffers");
    }
    request.arguments = {"args", "", "two words", "a\"b", "slash\\", "中文", "& | ; $(not-a-shell)"};
    {
        LineProcess child(request);
        for (std::size_t i = 1; i < request.arguments.size(); ++i)
            check(child.receive(Clock::now() + 3s, cancelled) ==
                std::to_string(request.arguments[i].size()) + ':' + request.arguments[i], "direct arguments preserve quoting");
    }
    request.arguments = {"stderr"};
    {
        LineProcess child(request);
        child.send("protocol-only", Clock::now() + 5s, cancelled);
        check(child.receive(Clock::now() + 5s, cancelled) == "protocol-only", "stderr flood drains separately without leaking");
    }
    request.arguments = {"crlf"};
    {
        LineProcess child(request);
        check(child.receive(Clock::now() + 3s, cancelled) == "first", "CRLF framing supported");
        check(child.receive(Clock::now() + 3s, cancelled) == "second", "queued second line preserved");
    }
    for (const auto* mode : {"exit", "partial", "invalid"}) {
        request.arguments = {mode};
        LineProcess child(request);
        rejects([&] { child.receive(Clock::now() + 3s, cancelled); }, "EOF, partial line and invalid UTF-8 fail");
        rejects([&] { child.send("after-error", Clock::now() + 3s, cancelled); }, "transport failure terminates child");
    }
    for (const auto* mode : {"flood", "queued"}) {
        request.arguments = {mode};
        request.max_output_bytes = 4096;
        LineProcess child(request);
        // The background reader must bound queued output even with no receive call.
        std::this_thread::sleep_for(200ms);
        rejects([&] { child.receive(Clock::now() + 3s, cancelled); }, "stdout and queued-line floods bounded", "limit");
    }
    request.max_output_bytes = 1024 * 1024;
    request.arguments = {"hang"};
    {
        LineProcess child(request);
        const auto start = Clock::now();
        rejects([&] { child.receive(start + 100ms, cancelled); }, "receive deadline", "timed out");
        check(Clock::now() - start < 2s, "receive timeout teardown bounded");
    }
    {
        LineProcess child(request);
        const auto start = Clock::now();
        rejects([&] { child.send(std::string(512 * 1024, 'x'), start + 100ms, cancelled); },
                "nonreading stdin deadline", "timed out");
        check(Clock::now() - start < 2s, "blocked write teardown bounded");
    }
    {
        LineProcess child(request);
        std::jthread cancel([&] { std::this_thread::sleep_for(100ms); cancelled.store(true); });
        const auto start = Clock::now();
        rejects([&] { child.receive(start + 5s, cancelled); }, "receive cancellation", "cancelled");
        check(Clock::now() - start < 2s, "cancellation bounded");
        cancel.join();
        cancelled.store(false);
    }
    {
        LineProcess child(request);
        std::jthread cancel([&] { std::this_thread::sleep_for(100ms); cancelled.store(true); });
        rejects([&] { child.send(std::string(512 * 1024, 'x'), Clock::now() + 5s, cancelled); },
                "blocked write cancellation", "cancelled");
        cancel.join();
        cancelled.store(false);
    }
    request.arguments = {"echo"};
    for (const auto& invalid : {std::string("one\ntwo"), std::string("one\rtwo"), std::string("a\0b", 3), std::string("\xc0\xaf", 2)}) {
        LineProcess child(request);
        rejects([&] { child.send(invalid, Clock::now() + 1s, cancelled); }, "invalid outgoing framing/encoding rejected");
    }
    {
        LineProcess child(request);
        cancelled.store(true);
        rejects([&] { child.send("already-cancelled", Clock::now() + 1s, cancelled); }, "pre-cancelled send", "cancelled");
        cancelled.store(false);
    }
    {
        ScopedEnvironment first("CODESTUDIO_API_KEY"), second("OPENAI_API_KEY"), third("ANTHROPIC_API_KEY"),
            cloud("AWS_SECRET_ACCESS_KEY"), git("GITHUB_TOKEN"), npm("NPM_TOKEN"), arbitrary("UNRELATED_CUSTOM_SECRET");
        request.arguments = {"env"};
        LineProcess child(request);
        for (int i = 0; i < 7; ++i)
            check(child.receive(Clock::now() + 3s, cancelled).ends_with(":absent"), "arbitrary credential variables not inherited");
    }
    const auto directory = std::filesystem::temp_directory_path() /
        ("codestudio-line-process-" + std::to_string(std::random_device{}()) + '-' + std::to_string(Clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(directory), "unique fixture directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    const auto marker = directory / "descendant marker.txt";
    const auto marker_utf8 = marker.u8string();
    request.arguments = {"child", std::string(marker_utf8.begin(), marker_utf8.end())};
    {
        LineProcess child(request);
        check(child.receive(Clock::now() + 3s, cancelled) == "descendant-started", "descendant fixture launched");
    }
    std::this_thread::sleep_for(1s);
    check(!std::filesystem::exists(marker), "destructor kills descendants after parent exits");

    request.arguments = {"echo"};
    request.executable = fixture.filename();
    rejects([&] { LineProcess child(request); }, "relative executable rejected");
    request.executable = fixture;
    request.arguments = {std::string("bad\0argument", 12)};
    rejects([&] { LineProcess child(request); }, "NUL argument rejected");
    request.arguments = {"echo"};
    request.max_output_bytes = 0;
    rejects([&] { LineProcess child(request); }, "zero buffer limit rejected");
}
