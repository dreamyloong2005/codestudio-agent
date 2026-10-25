#include "codestudio/scheduler_child.hpp"

#include <charconv>
#include <chrono>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace codestudio {
namespace {
void validate_options(const SchedulerChildOptions& options) {
    for (const auto* path : {&options.workspace, &options.database}) {
        const auto& value = path->native();
        if (value.empty() || value.size() >= 32767 ||
            value.find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos || !path->is_absolute())
            throw std::invalid_argument("Scheduler child workspace and state must be bounded absolute paths without NUL");
    }
    if (options.owner.empty() || options.owner.size() > 128)
        throw std::invalid_argument("Scheduler child owner token must contain 1..128 characters");
    for (const char ch : options.owner)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            throw std::invalid_argument("Invalid scheduler child owner token");
    if (options.poll_ms < 20 || options.poll_ms > 5000)
        throw std::invalid_argument("Scheduler child poll-ms must be in 20..5000");
    if (options.expected_stop_epoch < 0 || options.expected_generation < 0)
        throw std::invalid_argument("Scheduler child expected stop epoch and generation must be nonnegative");
    if (options.expected_generation == std::numeric_limits<std::int64_t>::max())
        throw std::invalid_argument("Scheduler child expected generation cannot be incremented");
}

#ifdef _WIN32
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    HANDLE release() { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

[[noreturn]] void windows_error(const char* operation, DWORD code = GetLastError()) {
    throw std::system_error(static_cast<int>(code), std::system_category(), operation);
}

std::wstring current_executable() {
    std::vector<wchar_t> path(512);
    for (;;) {
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size) windows_error("GetModuleFileNameW(scheduler child)");
        if (size < path.size()) {
            std::wstring result(path.data(), size);
            if (!std::filesystem::path(result).is_absolute())
                throw std::runtime_error("Current scheduler executable path is not absolute");
            return result;
        }
        if (path.size() == 32768) throw std::runtime_error("Scheduler executable path exceeds its limit");
        path.resize(path.size() * 2);
    }
}

std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

class ProcessAttributes {
public:
    ProcessAttributes(HANDLE inherited, HANDLE job) : inherited_(inherited), job_(job) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        memory_ = HeapAlloc(GetProcessHeap(), 0, size);
        if (!memory_) throw std::bad_alloc();
        if (!InitializeProcThreadAttributeList(get(), 2, 0, &size)) {
            const auto error = GetLastError();
            HeapFree(GetProcessHeap(), 0, memory_);
            memory_ = nullptr;
            windows_error("InitializeProcThreadAttributeList(scheduler child)", error);
        }
        if (!UpdateProcThreadAttribute(get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       &inherited_, sizeof(inherited_), nullptr, nullptr) ||
            !UpdateProcThreadAttribute(get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                       &job_, sizeof(job_), nullptr, nullptr)) {
            const auto error = GetLastError();
            DeleteProcThreadAttributeList(get());
            HeapFree(GetProcessHeap(), 0, memory_);
            memory_ = nullptr;
            windows_error("UpdateProcThreadAttribute(scheduler child)", error);
        }
    }
    ~ProcessAttributes() {
        if (memory_) {
            DeleteProcThreadAttributeList(get());
            HeapFree(GetProcessHeap(), 0, memory_);
        }
    }
    ProcessAttributes(const ProcessAttributes&) = delete;
    ProcessAttributes& operator=(const ProcessAttributes&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const {
        return static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory_);
    }
private:
    HANDLE inherited_;
    HANDLE job_;
    void* memory_ = nullptr;
};

class WindowsSchedulerChild final : public SchedulerChild {
public:
    explicit WindowsSchedulerChild(HANDLE job) : job_(job) {}
    ~WindowsSchedulerChild() override {
        // Closing the private Job also reclaims descendants after the worker
        // exits. Creation-time assignment guarantees every adopted process is
        // already in this Job. Wait even if asynchronous termination is pending.
        job_.reset();
        if (process_.get()) WaitForSingleObject(process_.get(), INFINITE);
    }
    void adopt(HANDLE process, DWORD pid) noexcept { process_.reset(process); pid_ = pid; }
    HANDLE job() const { return job_.get(); }
    std::uint32_t pid() const override { return static_cast<std::uint32_t>(pid_); }
    std::optional<std::int64_t> poll_exit() override {
        if (exit_) return exit_;
        const auto ready = WaitForSingleObject(process_.get(), 0);
        if (ready == WAIT_TIMEOUT) return std::nullopt;
        if (ready != WAIT_OBJECT_0) windows_error("WaitForSingleObject(scheduler child)");
        DWORD code = 0;
        if (!GetExitCodeProcess(process_.get(), &code)) windows_error("GetExitCodeProcess(scheduler child)");
        exit_ = static_cast<std::int64_t>(code);
        return exit_;
    }
private:
    Handle job_;
    Handle process_;
    DWORD pid_ = 0;
    std::optional<std::int64_t> exit_;
};
#elif defined(__linux__)
class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
    void reset(int value = -1) { if (value_ >= 0) close(value_); value_ = value; }
private:
    int value_;
};

[[noreturn]] void posix_error(const char* operation, int error = errno) {
    throw std::system_error(error, std::generic_category(), operation);
}

class LinuxSchedulerChild final : public SchedulerChild {
public:
    ~LinuxSchedulerChild() override {
        if (!active_) return;
        int status = 0;
        pid_t result;
        do { result = waitpid(pid_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result == pid_ || (result < 0 && errno == ECHILD)) return;
        // The direct child remains unreaped, so its PID cannot be reused.
        if (kill(pid_, SIGKILL) == 0 || errno == ESRCH)
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    }
    void adopt(pid_t pid) noexcept { pid_ = pid; active_ = true; }
    std::uint32_t pid() const override { return static_cast<std::uint32_t>(pid_); }
    std::optional<std::int64_t> poll_exit() override {
        if (exit_) return exit_;
        if (!active_) throw std::runtime_error("Scheduler child is no longer owned");
        int status = 0;
        pid_t result;
        do { result = waitpid(pid_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
        if (!result) return std::nullopt;
        if (result < 0) {
            const auto error = errno;
            // Never signal a recorded PID if another reaper has taken ownership.
            if (error == ECHILD) active_ = false;
            posix_error("waitpid(scheduler child)", error);
        }
        if (WIFEXITED(status)) exit_ = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_ = 128 + WTERMSIG(status);
        else return std::nullopt;
        active_ = false;
        return exit_;
    }
private:
    pid_t pid_ = -1;
    bool active_ = false;
    std::optional<std::int64_t> exit_;
};

void ensure_nonstandard(Descriptor& descriptor) {
    if (descriptor.get() >= 3) return;
    const auto duplicate = fcntl(descriptor.get(), F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0) posix_error("Reserve scheduler child descriptor");
    descriptor.reset(duplicate);
}
#endif
}

std::unique_ptr<SchedulerChild> start_scheduler_child(const SchedulerChildOptions& options) {
    validate_options(options);
#ifdef _WIN32
    const auto executable = current_executable();
    std::wstring command = quote(executable) + L" scheduler-child --workspace " + quote(options.workspace.native()) +
        L" --state " + quote(options.database.native()) + L" --owner " +
        quote(std::wstring(options.owner.begin(), options.owner.end())) +
        L" --poll-ms " + std::to_wstring(options.poll_ms) +
        L" --expected-stop-epoch " + std::to_wstring(options.expected_stop_epoch) +
        L" --expected-generation " + std::to_wstring(options.expected_generation);
    if (command.size() >= 32767) throw std::invalid_argument("Scheduler child command line exceeds its limit");

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.get()) windows_error("CreateJobObjectW(scheduler child)");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        windows_error("SetInformationJobObject(scheduler child)");
    auto child = std::make_unique<WindowsSchedulerChild>(job.get());
    (void)job.release();

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle null_io(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (null_io.get() == INVALID_HANDLE_VALUE) windows_error("CreateFileW(scheduler child NUL)");
    ProcessAttributes attributes(null_io.get(), child->job());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_io.get();
    startup.StartupInfo.hStdOutput = null_io.get();
    startup.StartupInfo.hStdError = null_io.get();
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION info{};
    // The private Job is assigned atomically during process creation, so even
    // abrupt parent death cannot leave an unowned suspended process behind.
    // No breakaway: a parent Job remains in force and the owned Job is nested.
    // The worker inherits credentials in the environment, never client pipes.
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, options.workspace.c_str(), &startup.StartupInfo, &info))
        windows_error("CreateProcessW(scheduler child)");
    child->adopt(info.hProcess, info.dwProcessId);
    Handle thread(info.hThread);
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
        windows_error("ResumeThread(scheduler child)");
    return child;
#elif defined(__linux__)
    // Execute the mapped image even if its on-disk pathname was replaced or
    // removed. No path supplied by the caller selects the worker executable.
    const std::string executable = "/proc/self/exe";
    std::vector<std::string> arguments{executable, "scheduler-child", "--workspace", options.workspace.string(),
        "--state", options.database.string(), "--owner", options.owner, "--poll-ms", std::to_string(options.poll_ms),
        "--expected-stop-epoch", std::to_string(options.expected_stop_epoch),
        "--expected-generation", std::to_string(options.expected_generation)};
    std::vector<char*> raw;
    raw.reserve(arguments.size() + 1);
    for (auto& argument : arguments) raw.push_back(argument.data());
    raw.push_back(nullptr);

    struct rlimit descriptors{};
    if (getrlimit(RLIMIT_NOFILE, &descriptors) != 0) posix_error("Read scheduler child descriptor limit");
    auto descriptor_limit = descriptors.rlim_cur == RLIM_INFINITY ? static_cast<rlim_t>(INT_MAX) : descriptors.rlim_cur;
    // A descriptor may still be open above a subsequently lowered resource limit.
    // Discover the fallback bound before forking from a multithreaded service.
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        const auto name = entry.path().filename().string();
        unsigned descriptor = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), descriptor);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() && descriptor < INT_MAX &&
            static_cast<rlim_t>(descriptor) + 1 > descriptor_limit)
            descriptor_limit = static_cast<rlim_t>(descriptor) + 1;
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) posix_error("Create scheduler child exec pipe");
    Descriptor receipt_read(pipefd[0]), receipt_write(pipefd[1]);
    ensure_nonstandard(receipt_read);
    ensure_nonstandard(receipt_write);
    Descriptor null_io(open("/dev/null", O_RDWR | O_CLOEXEC));
    if (null_io.get() < 0) posix_error("Open scheduler child null streams");
    ensure_nonstandard(null_io);
    auto child = std::make_unique<LinuxSchedulerChild>();
    const auto parent_pid = getpid();
    const auto directory = options.workspace.string();
    const auto pid = fork();
    if (pid < 0) posix_error("Fork scheduler child");
    if (!pid) {
        // Only async-signal-safe operations and preallocated memory after fork.
        const auto fail = [&](int error) {
            ssize_t written;
            do { written = write(receipt_write.get(), &error, sizeof(error)); } while (written < 0 && errno == EINTR);
            _exit(127);
        };
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) fail(errno);
        if (getppid() != parent_pid) fail(ECANCELED);
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        for (int signal = 1; signal < NSIG; ++signal) {
            if (signal == SIGKILL || signal == SIGSTOP) continue;
            if (sigaction(signal, &action, nullptr) != 0 && errno != EINVAL) fail(errno);
        }
        sigset_t mask;
        sigemptyset(&mask);
        if (sigprocmask(SIG_SETMASK, &mask, nullptr) != 0 || chdir(directory.c_str()) != 0) fail(errno);
        for (int fd = 0; fd < 3; ++fd)
            if (dup2(null_io.get(), fd) < 0 || fcntl(fd, F_SETFD, 0) < 0) fail(errno);
        // Preserve only the CLOEXEC receipt until exec; all other descriptors,
        // including parent locks and transport pipes, are closed before launch.
        bool closed = false;
#ifdef SYS_close_range
        closed = (receipt_write.get() == 3 || syscall(SYS_close_range, 3U,
                    static_cast<unsigned>(receipt_write.get() - 1), 0U) == 0) &&
                 syscall(SYS_close_range, static_cast<unsigned>(receipt_write.get() + 1), UINT_MAX, 0U) == 0;
#endif
        if (!closed)
            for (rlim_t fd = 3; fd < descriptor_limit && fd <= INT_MAX; ++fd)
                if (static_cast<int>(fd) != receipt_write.get()) close(static_cast<int>(fd));
        execv(executable.c_str(), raw.data());
        fail(errno);
    }
    child->adopt(pid);
    receipt_write.reset();
    null_io.reset();
    int failure = 0;
    std::size_t received = 0;
    bool confirmed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd descriptor{receipt_read.get(), POLLIN | POLLHUP, 0};
        const auto ready = poll(&descriptor, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; posix_error("Poll scheduler child exec pipe"); }
        if (!ready) continue;
        const auto count = read(receipt_read.get(), reinterpret_cast<char*>(&failure) + received, sizeof(failure) - received);
        if (count < 0) { if (errno == EINTR) continue; posix_error("Read scheduler child exec pipe"); }
        if (!count) {
            if (received) posix_error("Truncated scheduler child exec receipt", EPROTO);
            confirmed = true;
            break;
        }
        received += static_cast<std::size_t>(count);
        if (received == sizeof(failure)) posix_error("Execute scheduler child", failure ? failure : EPROTO);
    }
    if (!confirmed) throw std::runtime_error("Scheduler child exec confirmation timed out");
    return child;
#else
    throw std::runtime_error("Owned scheduler children are supported only on Windows and Linux");
#endif
}
}
