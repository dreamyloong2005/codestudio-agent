#include "codestudio/types.hpp"
#include "codestudio/process_package.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "windows_sandbox.hpp"
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include "linux_sandbox.hpp"
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/close_range.h>
#endif
extern char** environ;
#endif

namespace codestudio {
namespace {
using Clock = std::chrono::steady_clock;

void validate_utf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first == 0) throw std::invalid_argument("Process arguments cannot contain NUL");
        if (first < 0x80) continue;
        unsigned code = 0;
        unsigned minimum = 0;
        int remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            code = first & 0x1f; remaining = 1; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            code = first & 0x0f; remaining = 2; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code = first & 0x07; remaining = 3; minimum = 0x10000;
        } else {
            throw std::invalid_argument("Process arguments must be valid UTF-8");
        }
        while (remaining-- > 0) {
            if (i == value.size()) throw std::invalid_argument("Truncated UTF-8 process argument");
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) throw std::invalid_argument("Invalid UTF-8 process argument");
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            throw std::invalid_argument("Invalid UTF-8 process argument");
        }
    }
}

void validate_request(const ProcessRequest& request) {
    validate_sandbox_mode(request.sandbox);
    validate_package_limits(request);
    const auto has_nul = [](const auto& path) {
        return path.native().find(typename std::filesystem::path::value_type{}) !=
               std::filesystem::path::string_type::npos;
    };
    if (has_nul(request.executable) || has_nul(request.working_directory)) {
        throw std::invalid_argument("Process paths cannot contain NUL");
    }
    if (!request.executable.is_absolute() || !std::filesystem::is_regular_file(request.executable)) {
        throw std::invalid_argument("Process executable must be an existing absolute file path");
    }
    if (!request.working_directory.is_absolute() || !std::filesystem::is_directory(request.working_directory)) {
        throw std::invalid_argument("Process working directory must be an existing absolute directory");
    }
    if (request.timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("Process timeout must be positive");
    }
    for (const auto& argument : request.arguments) validate_utf8(argument);
}

void append_output(ProcessResult& result, const char* data, std::size_t count, std::size_t limit) {
    const auto retained = std::min(count, limit - result.output.size());
    result.output.append(data, retained);
    result.output_truncated = result.output_truncated || retained < count;
}

#ifdef _WIN32
class Handle {
public:
    explicit Handle(HANDLE handle = nullptr) : handle_(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return handle_; }
    void reset(HANDLE handle = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }
private:
    HANDLE handle_;
};

[[noreturn]] void fail_windows(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

std::wstring to_wide(const std::string& value) {
    if (value.empty()) return {};
    if (value.size() > 32767) throw std::invalid_argument("Process argument is too long");
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (!length) fail_windows("MultiByteToWideChar");
    std::wstring result(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length)) {
        fail_windows("MultiByteToWideChar");
    }
    return result;
}

// Quote for CommandLineToArgvW / the Windows CRT, including empty arguments and trailing slashes.
std::wstring quote_argument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : argument) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted += ch;
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

std::vector<wchar_t> child_environment() {
    struct Environment {
        wchar_t* value = GetEnvironmentStringsW();
        ~Environment() { if (value) FreeEnvironmentStringsW(value); }
    } source;
    if (!source.value) fail_windows("GetEnvironmentStringsW");
    std::vector<wchar_t> result;
    for (auto* entry = source.value; *entry; entry += std::wcslen(entry) + 1) {
        const auto* equals = std::wcschr(entry, L'=');
        const auto length = equals ? static_cast<int>(equals - entry) : 0;
        const auto secret = [&](const wchar_t* name) {
            return length > 0 && CompareStringOrdinal(entry, length, name, -1, TRUE) == CSTR_EQUAL;
        };
        if (secret(L"CODESTUDIO_API_KEY") || secret(L"OPENAI_API_KEY")) continue;
        result.insert(result.end(), entry, entry + std::wcslen(entry) + 1);
    }
    result.push_back(L'\0');
    if (result.size() == 1) result.push_back(L'\0');
    return result;
}

class Attributes {
public:
    explicit Attributes(DWORD count = 1) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, count, 0, &size);
        memory_ = HeapAlloc(GetProcessHeap(), 0, size);
        if (!memory_) throw std::bad_alloc();
        if (!InitializeProcThreadAttributeList(get(), count, 0, &size)) {
            const auto error = GetLastError();
            HeapFree(GetProcessHeap(), 0, memory_);
            memory_ = nullptr;
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "InitializeProcThreadAttributeList");
        }
    }
    ~Attributes() {
        if (memory_) {
            DeleteProcThreadAttributeList(get());
            HeapFree(GetProcessHeap(), 0, memory_);
        }
    }
    Attributes(const Attributes&) = delete;
    Attributes& operator=(const Attributes&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const {
        return static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory_);
    }
private:
    void* memory_ = nullptr;
};
#else
class Descriptor {
public:
    explicit Descriptor(int fd = -1) : fd_(fd) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
    void reset(int fd = -1) {
        if (fd_ >= 0) close(fd_);
        fd_ = fd;
    }
private:
    int fd_;
};

[[noreturn]] void fail_posix(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void make_pipe(int (&fds)[2]) {
    if (pipe(fds) != 0) fail_posix("pipe");
#ifdef __linux__
    for (auto& fd : fds) if (fd < 3) {
        const auto duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) {
            const auto error = errno;
            close(fds[0]); close(fds[1]);
            throw std::system_error(error, std::generic_category(), "fcntl(F_DUPFD_CLOEXEC)");
        }
        close(fd); fd = duplicate;
    }
#endif
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) == -1 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) == -1) {
        const auto error = errno;
        close(fds[0]); close(fds[1]);
        throw std::system_error(error, std::generic_category(), "fcntl(FD_CLOEXEC)");
    }
}
#endif
} // namespace

static ProcessResult run_process_impl(const ProcessRequest& request, const std::atomic_bool& cancelled
#ifdef _WIN32
                                     , WindowsSandbox* supplied_sandbox = nullptr
#elif defined(__linux__)
                                     , LinuxSandbox* supplied_sandbox = nullptr
#endif
                                     ) {
    validate_request(request);
    ProcessResult result;
    if (cancelled.load()) {
        result.cancelled = true;
        return result;
    }
    const auto started = Clock::now();
#ifdef _WIN32
    std::unique_ptr<WindowsSandbox> owned_sandbox;
    auto* sandbox = supplied_sandbox;
    if (!sandbox && request.sandbox == SandboxMode::appcontainer) {
        owned_sandbox = std::make_unique<WindowsSandbox>(request,nullptr,&cancelled);
        sandbox = owned_sandbox.get();
    }
    const auto interrupted = [&] {
        result.cancelled = cancelled.load();
        result.timed_out = !result.cancelled && Clock::now() - started >= request.timeout;
        return result.cancelled || result.timed_out;
    };
    if (interrupted()) return result;
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.get()) fail_windows("CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &job_limits, sizeof(job_limits))) {
        fail_windows("SetInformationJobObject");
    }
    if (sandbox) sandbox->supervise_job(job.get());
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_raw = nullptr, write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &security, 65536)) fail_windows("CreatePipe");
    Handle output_read(read_raw), output_write(write_raw);
    if (!SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0)) fail_windows("SetHandleInformation");
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) fail_windows("CreateFileW(NUL)");
    Attributes attributes(1 + (sandbox ? WindowsSandbox::extra_attribute_count : 0));
    if (sandbox) sandbox->apply_attributes(attributes.get());
    std::array<HANDLE, 2> inherited{input.get(), output_write.get()};
    if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof(inherited), nullptr, nullptr)) {
        fail_windows("UpdateProcThreadAttribute");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output_write.get();
    startup.StartupInfo.hStdError = output_write.get();
    startup.lpAttributeList = attributes.get();
    const auto& executable = sandbox ? sandbox->executable() : request.executable;
    const auto& directory = sandbox ? sandbox->working_directory() : request.working_directory;
    auto command = quote_argument(executable.native());
    const auto& sandbox_arguments=sandbox && request.package ? sandbox->arguments() : request.arguments;
    for (const auto& argument : sandbox_arguments) command += L" " + quote_argument(to_wide(argument));
    if (command.size() >= 32767) throw std::invalid_argument("Process command line is too long");
    auto environment = sandbox ? sandbox->environment() : child_environment();
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        environment.data(), directory.c_str(), &startup.StartupInfo, &info)) {
        fail_windows("CreateProcessW");
    }
    Handle process(info.hProcess), thread(info.hThread);
    if (!sandbox && !AssignProcessToJobObject(job.get(), process.get())) {
        const auto error = GetLastError();
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), INFINITE);
        throw std::system_error(static_cast<int>(error), std::system_category(), "AssignProcessToJobObject");
    }
    if (sandbox) sandbox->verify(process.get());
    if (interrupted()) return result; // The suspended child is reclaimed without running user code.
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) fail_windows("ResumeThread");
    thread.reset();
    output_write.reset();
    input.reset();
    std::array<char, 8192> buffer{};
    bool terminated = false;
    bool pipe_closed = false;
    for (;;) {
        bool received_output = false;
        // A continuously writing process must not prevent cancellation/deadline checks.
        for (std::size_t drained = 0; drained < 262144 && !pipe_closed;) {
            DWORD available = 0;
            if (!PeekNamedPipe(output_read.get(), nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) { pipe_closed = true; break; }
                fail_windows("PeekNamedPipe");
            }
            if (!available) break;
            DWORD count = 0;
            if (!ReadFile(output_read.get(), buffer.data(),
                          std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) { pipe_closed = true; break; }
                fail_windows("ReadFile");
            }
            append_output(result, buffer.data(), count, request.max_output_bytes);
            drained += count;
            received_output = received_output || count != 0;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation,
                                        &accounting, sizeof(accounting), nullptr)) {
            fail_windows("QueryInformationJobObject");
        }
        if (accounting.ActiveProcesses == 0 && pipe_closed) break;
        if (!terminated && (cancelled.load() || Clock::now() - started >= request.timeout)) {
            result.cancelled = cancelled.load();
            result.timed_out = !result.cancelled;
            if (!TerminateJobObject(job.get(), 1)) fail_windows("TerminateJobObject");
            terminated = true;
        }
        // Sleeping after every small anonymous-pipe buffer can throttle output below
        // the deadline on Windows (whose default timer tick is commonly ~15 ms).
        if (received_output) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process.get(), &code)) fail_windows("GetExitCodeProcess");
    result.exit_code = static_cast<int>(code);
#else
    std::vector<std::string> arguments{request.executable.string()};
    arguments.insert(arguments.end(), request.arguments.begin(), request.arguments.end());
#ifdef __linux__
    std::unique_ptr<LinuxSandbox> owned_sandbox;
    auto* sandbox = supplied_sandbox;
    if (!sandbox && request.sandbox == SandboxMode::linux_namespace) {
        owned_sandbox = std::make_unique<LinuxSandbox>(request,nullptr,&cancelled);
        sandbox = owned_sandbox.get();
    }
    if (sandbox) arguments = sandbox->arguments(request);
#endif
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (auto** entry = environ; entry && *entry; ++entry) {
        const std::string value(*entry);
        if (!value.starts_with("CODESTUDIO_API_KEY=") && !value.starts_with("OPENAI_API_KEY=")) {
            environment.push_back(value);
        }
    }
#ifdef __linux__
    if (sandbox) environment = {"PATH=/usr/bin", "LANG=C.UTF-8"};
#endif
    std::vector<char*> envp;
    for (auto& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    int output_fds[2], error_fds[2];
    make_pipe(output_fds);
    Descriptor output_read(output_fds[0]), output_write(output_fds[1]);
    make_pipe(error_fds);
    Descriptor error_read(error_fds[0]), error_write(error_fds[1]);
    Descriptor input(open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (input.get() < 0) fail_posix("open(/dev/null)");
#ifdef __linux__
    if (input.get() < 3) {
        const auto duplicate = fcntl(input.get(), F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) fail_posix("fcntl(F_DUPFD_CLOEXEC input)");
        input.reset(duplicate);
    }
#endif
    if (fcntl(output_read.get(), F_SETFL, O_NONBLOCK) == -1) fail_posix("fcntl(O_NONBLOCK)");
    result.cancelled = cancelled.load();
    result.timed_out = !result.cancelled && Clock::now() - started >= request.timeout;
    if (result.cancelled || result.timed_out) return result;
    const auto parent_pid = getpid();
    const auto pid = fork();
    if (pid < 0) fail_posix("fork");
    if (pid == 0) {
        const auto child_error = [&](int error) {
            const auto ignored = write(error_write.get(), &error, sizeof(error));
            (void)ignored;
            _exit(127);
        };
        close(error_read.get());
        close(output_read.get());
        if (setpgid(0, 0) != 0 || chdir(request.working_directory.c_str()) != 0 ||
            dup2(input.get(), STDIN_FILENO) < 0 || dup2(output_write.get(), STDOUT_FILENO) < 0 ||
            dup2(output_write.get(), STDERR_FILENO) < 0) child_error(errno);
        close(input.get());
        close(output_write.get());
#ifdef __linux__
        if (sandbox) {
            if (prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent_pid) child_error(ECANCELED);
            if (syscall(SYS_close_range,3U,~0U,CLOSE_RANGE_CLOEXEC)<0) child_error(errno);
            if (const auto error=sandbox->child_setup()) child_error(error);
        }
#endif
        execve(arguments.front().c_str(), argv.data(), envp.data());
        child_error(errno);
    }
    struct ChildGuard {
        pid_t pid;
#ifdef __linux__
        LinuxSandbox* sandbox;
#endif
        bool reaped = false;
        bool complete = false;
        ~ChildGuard() {
            if (complete) return;
#ifdef __linux__
            if (sandbox) sandbox->terminate();
#endif
            kill(-pid, SIGKILL);
            if (!reaped) {
                kill(pid, SIGKILL);
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
        }
    } child{pid
#ifdef __linux__
        ,sandbox
#endif
    };
    // Both parent and child set the group to remove the fork/exec race.
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) fail_posix("setpgid");
    error_write.reset();
    output_write.reset();
    input.reset();
    int launch_error = 0;
    ssize_t error_count;
    do { error_count = read(error_read.get(), &launch_error, sizeof(launch_error)); }
    while (error_count < 0 && errno == EINTR);
    if (error_count < 0) fail_posix("read(exec status)");
    if (error_count > 0) throw std::system_error(launch_error, std::generic_category(), "execve");
    error_read.reset();
    std::array<char, 8192> buffer{};
    bool terminated = false, pipe_closed = false;
    int status = 0;
    for (;;) {
        bool received_output = false;
        for (std::size_t drained = 0; drained < 262144 && !pipe_closed;) {
            const auto count = read(output_read.get(), buffer.data(), buffer.size());
            if (count > 0) {
                append_output(result, buffer.data(), static_cast<std::size_t>(count), request.max_output_bytes);
                drained += static_cast<std::size_t>(count);
                received_output = true;
            } else if (count == 0) { pipe_closed = true; }
            else if (errno == EINTR) { continue; }
            else if (errno == EAGAIN || errno == EWOULDBLOCK) { break; }
            else { fail_posix("read(process output)"); }
        }
        if (!child.reaped) {
            const auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) child.reaped = true;
            else if (waited < 0 && errno != EINTR) fail_posix("waitpid");
        }
        const bool group_alive =
#ifdef __linux__
            sandbox ? sandbox->populated() :
#endif
            (kill(-pid, 0) == 0 || errno == EPERM);
        if (child.reaped && pipe_closed && (terminated || !group_alive)) break;
        if (!terminated && (cancelled.load() || Clock::now() - started >= request.timeout)) {
            result.cancelled = cancelled.load();
            result.timed_out = !result.cancelled;
#ifdef __linux__
            if (sandbox) sandbox->terminate();
#endif
            if (kill(-pid, SIGKILL) < 0 && errno != ESRCH) fail_posix("kill(process group)");
            terminated = true;
        }
        if (received_output) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#ifdef __linux__
    if (sandbox) sandbox->quiesce();
#endif
    child.complete = true;
#endif
    return result;
}
ProcessResult run_process(const ProcessRequest& request, const std::atomic_bool& cancelled) {
    return run_process_impl(request, cancelled);
}
#ifdef _WIN32
ProcessResult run_build_process(const ProcessRequest& request, const std::atomic_bool& cancelled, WindowsSandbox& sandbox) {
    return run_process_impl(request, cancelled, &sandbox);
}
#elif defined(__linux__)
ProcessResult run_build_process(const ProcessRequest& request, const std::atomic_bool& cancelled, LinuxSandbox& sandbox) {
    return run_process_impl(request, cancelled, &sandbox);
}
#endif
} // namespace codestudio
