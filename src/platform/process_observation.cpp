#include "codestudio/process_observation.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace codestudio {
namespace {
#ifdef _WIN32
class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

bool parse_identity(std::string_view identity, std::uint64_t& creation) {
    if (identity.size() != 24 || !identity.starts_with("windows:")) return false;
    const auto digits = identity.substr(8);
    for (const auto c : digits)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), creation, 16);
    return parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() && creation != 0;
}

bool process_creation(HANDLE process, std::uint64_t& creation) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
    creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return creation != 0;
}

std::string encode_identity(std::uint64_t creation) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "windows:0000000000000000";
    for (std::size_t i = result.size(); i > 8; --i) {
        result[i - 1] = digits[creation & 15];
        creation >>= 4;
    }
    return result;
}
#elif defined(__linux__)
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

bool unsigned_decimal(std::string_view text, std::uint64_t& value) {
    if (text.empty() || text.size() > 20 || text.front() < '1' || text.front() > '9') return false;
    for (const auto c : text) if (c < '0' || c > '9') return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool boot_uuid(std::string_view text) {
    if (text.size() != 36) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            nonzero = nonzero || c != '0';
        }
    }
    return nonzero;
}

bool parse_identity(std::string_view identity, std::string_view& boot, std::uint64_t& start) {
    if (identity.size() < 44 || identity.size() > 63 || !identity.starts_with("linux:") || identity[42] != ':')
        return false;
    boot = identity.substr(6, 36);
    return boot_uuid(boot) && unsigned_decimal(identity.substr(43), start);
}

bool read_small_file(const std::string& path, std::size_t maximum, std::string& output) {
    const Descriptor file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0) return false;
    output.clear();
    std::array<char, 512> buffer{};
    for (;;) {
        const auto count = read(file.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return true;
        if (static_cast<std::size_t>(count) > maximum - output.size()) return false;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

bool current_boot(std::string& boot) {
    if (!read_small_file("/proc/sys/kernel/random/boot_id", 37, boot)) return false;
    if (boot.size() == 37 && boot.back() == '\n') boot.pop_back();
    return boot_uuid(boot);
}

bool process_start(std::int64_t pid, std::uint64_t& start, bool& exited) {
    std::string stat;
    if (!read_small_file("/proc/" + std::to_string(pid) + "/stat", 8192, stat)) return false;
    const auto prefix = std::to_string(pid) + " (";
    if (!stat.starts_with(prefix)) return false;
    // comm can contain spaces, newlines and right parentheses. Numeric fields
    // after comm contain no parentheses, so only the last ')' closes field 2.
    const auto close = stat.rfind(')');
    if (close == std::string::npos || close < prefix.size() || close + 4 >= stat.size() ||
        stat[close + 1] != ' ' || stat[close + 3] != ' ') return false;
    const auto state = stat[close + 2];
    if (std::string_view("RSDZTtXxKWPIN").find(state) == std::string_view::npos) return false;
    exited = state == 'Z' || state == 'X' || state == 'x';
    std::string_view fields(stat.data() + close + 4, stat.size() - close - 4);
    for (int field = 4; field <= 22; ++field) {
        const auto end = fields.find(' ');
        if (end == std::string_view::npos || end == 0) return false;
        if (field == 22) return unsigned_decimal(fields.substr(0, end), start);
        fields.remove_prefix(end + 1);
    }
    return false;
}

// 0 means the bound process was not observed to have exited; 1 means exited;
// -1 means observation was unavailable. No signal is ever sent.
int pidfd_state(int fd) {
    pollfd descriptor{fd, POLLIN, 0};
    int result;
    do { result = poll(&descriptor, 1, 0); } while (result < 0 && errno == EINTR);
    if (result < 0 || (descriptor.revents & (POLLERR | POLLNVAL))) return -1;
    if (descriptor.revents & (POLLIN | POLLHUP)) return 1;
    return result == 0 ? 0 : -1;
}
#endif
}

std::string current_process_identity() {
#ifdef _WIN32
    std::uint64_t creation = 0;
    if (!process_creation(GetCurrentProcess(), creation))
        throw std::runtime_error("Cannot read the current Windows process creation time");
    return encode_identity(creation);
#elif defined(__linux__)
    std::string boot;
    std::uint64_t start = 0;
    bool exited = false;
    if (!current_boot(boot) || !process_start(static_cast<std::int64_t>(getpid()), start, exited) || exited)
        throw std::runtime_error("Cannot read the current Linux process birth identity");
    return "linux:" + boot + ":" + std::to_string(start);
#else
    throw std::runtime_error("Process identity is unavailable on this platform");
#endif
}

std::string observe_process_identity(std::int64_t pid, const std::string& identity) {
    try {
#ifdef _WIN32
        std::uint64_t expected_creation = 0;
        if (pid <= 0 || pid > std::numeric_limits<DWORD>::max() || !parse_identity(identity, expected_creation))
            return "unavailable";
        const Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                         static_cast<DWORD>(pid)));
        if (!process.get())
            return GetLastError() == ERROR_INVALID_PARAMETER ? "not_running" : "unavailable";
        auto state = WaitForSingleObject(process.get(), 0);
        if (state == WAIT_OBJECT_0) return "not_running";
        if (state != WAIT_TIMEOUT) return "unavailable";
        std::uint64_t actual_creation = 0;
        if (!process_creation(process.get(), actual_creation)) return "unavailable";
        state = WaitForSingleObject(process.get(), 0);
        if (state == WAIT_OBJECT_0) return "not_running";
        if (state != WAIT_TIMEOUT) return "unavailable";
        return actual_creation == expected_creation ? "matching_process" : "identity_mismatch";
#elif defined(__linux__)
        std::string_view expected_boot;
        std::uint64_t expected_start = 0;
        if (pid <= 0 || pid > std::numeric_limits<pid_t>::max() ||
            !parse_identity(identity, expected_boot, expected_start)) return "unavailable";
#ifdef SYS_pidfd_open
        const Descriptor process(static_cast<int>(syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0)));
        if (process.get() < 0) return errno == ESRCH ? "not_running" : "unavailable";
        auto state = pidfd_state(process.get());
        if (state != 0) return state == 1 ? "not_running" : "unavailable";
        std::string actual_boot;
        std::uint64_t actual_start = 0;
        bool exited = false;
        const bool readable = current_boot(actual_boot) && process_start(pid, actual_start, exited);
        // Check the same pidfd after reading /proc. If the PID was reused while
        // reading, the originally bound process has exited and cannot match.
        state = pidfd_state(process.get());
        if (state != 0) return state == 1 ? "not_running" : "unavailable";
        if (!readable) return "unavailable";
        if (exited) return "not_running";
        return actual_boot == expected_boot && actual_start == expected_start
            ? "matching_process" : "identity_mismatch";
#else
        return "unavailable";
#endif
#else
        (void)pid;
        (void)identity;
        return "unavailable";
#endif
    } catch (...) {
        return "unavailable";
    }
}
}
