#include "codestudio/process_observation.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("process observation: ") + message);
}
}

void run_process_observation_tests() {
    using namespace codestudio;
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
#ifdef _WIN32
    const auto pid = static_cast<std::int64_t>(GetCurrentProcessId());
#else
    const auto pid = static_cast<std::int64_t>(getpid());
#endif
    const auto identity = current_process_identity();
    check(identity == current_process_identity(), "current birth identity changed between reads");
    check(observe_process_identity(pid, identity) == "matching_process", "current process did not match its identity");
    auto changed = identity;
    changed.back() = changed.back() == '1' ? '2' : '1';
    check(observe_process_identity(pid, changed) == "identity_mismatch", "another birth time matched the current PID");
    check(observe_process_identity(std::numeric_limits<std::int32_t>::max(), identity) == "not_running",
          "absent PID was not reported as not running");
    check(observe_process_identity(0, identity) == "unavailable", "zero PID was accepted");
    check(observe_process_identity(-1, identity) == "unavailable", "negative PID was accepted");
    check(observe_process_identity(std::numeric_limits<std::int64_t>::max(), identity) == "unavailable",
          "out-of-range PID was accepted");
    for (const auto& invalid : {std::string{}, identity + "\n", identity + " ", identity + std::string(1, '\0'),
                               std::string(4096, 'a'), std::string("unknown:123")})
        check(observe_process_identity(pid, invalid) == "unavailable", "malformed identity was accepted");
#ifdef _WIN32
    check(observe_process_identity(pid, "windows:0000000000000000") == "unavailable", "zero creation time was accepted");
    check(observe_process_identity(pid, "windows:000000000000000A") == "unavailable", "noncanonical hex was accepted");
#else
    const auto separator = identity.rfind(':');
    check(observe_process_identity(pid, identity.substr(0, separator + 1) + "0") == "unavailable", "zero start time was accepted");
    check(observe_process_identity(pid, identity.substr(0, separator + 1) + "01") == "unavailable", "leading-zero start time was accepted");
    check(observe_process_identity(pid, identity.substr(0, separator + 1) + "18446744073709551616") == "unavailable",
          "overflowing start time was accepted");
    auto changed_boot = identity;
    const auto boot_offset = identity.find(':') + 1;
    changed_boot[boot_offset] = changed_boot[boot_offset] == '1' ? '2' : '1';
    check(observe_process_identity(pid, changed_boot) == "identity_mismatch", "another boot identity matched the current PID");
    // A child has a distinct birth identity, and its exit is reported without signalling it.
    int pipe_fds[2];
    check(pipe(pipe_fds) == 0, "child identity pipe creation failed");
    const auto child = fork();
    check(child >= 0, "child identity fork failed");
    if (child == 0) {
        close(pipe_fds[0]);
        try {
            const auto child_identity = current_process_identity();
            const auto count = write(pipe_fds[1], child_identity.data(), child_identity.size());
            close(pipe_fds[1]);
            _exit(count == static_cast<ssize_t>(child_identity.size()) ? 0 : 1);
        } catch (...) { _exit(2); }
    }
    close(pipe_fds[1]);
    std::string child_identity;
    char bytes[128];
    for (;;) {
        const auto count = read(pipe_fds[0], bytes, sizeof(bytes));
        if (count <= 0) break;
        child_identity.append(bytes, static_cast<std::size_t>(count));
    }
    close(pipe_fds[0]);
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child identity fixture failed");
    check(!child_identity.empty() && child_identity != identity, "child did not receive a distinct birth identity");
    check(observe_process_identity(child, child_identity) == "not_running", "exited child was reported as running");
#endif
#else
    bool rejected = false;
    try { current_process_identity(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "unsupported platform claimed a current process identity");
    check(observe_process_identity(1, "unsupported") == "unavailable", "unsupported platform claimed process observation");
#endif
}
