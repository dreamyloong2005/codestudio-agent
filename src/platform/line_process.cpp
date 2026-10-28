#include "codestudio/line_process.hpp"
#include "codestudio/process_package.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "windows_sandbox.hpp"
#else
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
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

void valid_utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        auto ch = static_cast<unsigned char>(value[i++]);
        if (!ch) throw std::invalid_argument("RPC text cannot contain NUL");
        if (ch < 0x80) continue;
        unsigned code, minimum;
        int count;
        if (ch >= 0xc2 && ch <= 0xdf) { code = ch & 0x1f; minimum = 0x80; count = 1; }
        else if (ch >= 0xe0 && ch <= 0xef) { code = ch & 0xf; minimum = 0x800; count = 2; }
        else if (ch >= 0xf0 && ch <= 0xf4) { code = ch & 7; minimum = 0x10000; count = 3; }
        else throw std::invalid_argument("RPC text must be valid UTF-8");
        while (count--) {
            if (i == value.size()) throw std::invalid_argument("Truncated UTF-8 RPC text");
            ch = static_cast<unsigned char>(value[i++]);
            if ((ch & 0xc0) != 0x80) throw std::invalid_argument("Invalid UTF-8 RPC text");
            code = (code << 6) | (ch & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            throw std::invalid_argument("Invalid UTF-8 RPC text");
    }
}

void validate(const ProcessRequest& request) {
    validate_sandbox_mode(request.sandbox);
    validate_package_limits(request);
    const auto has_nul = [](const std::filesystem::path& path) {
        return path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos;
    };
    if (has_nul(request.executable) || has_nul(request.working_directory))
        throw std::invalid_argument("RPC process paths cannot contain NUL");
    if (!request.executable.is_absolute() || !std::filesystem::is_regular_file(request.executable))
        throw std::invalid_argument("RPC executable must be an existing absolute file");
    if (!request.working_directory.is_absolute() || !std::filesystem::is_directory(request.working_directory))
        throw std::invalid_argument("RPC working directory must be an existing absolute directory");
    if (request.timeout <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("RPC startup timeout must be positive");
    if (!request.max_output_bytes || request.max_output_bytes > 64 * 1024 * 1024)
        throw std::invalid_argument("RPC output limit must be between 1 byte and 64 MiB");
    for (const auto& argument : request.arguments) valid_utf8(argument);
}

#ifdef _WIN32
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

[[noreturn]] void windows_error(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    if (value.size() > 32767) throw std::invalid_argument("RPC argument too long");
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (!length) windows_error("MultiByteToWideChar");
    std::wstring result(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length)) windows_error("MultiByteToWideChar");
    return result;
}

std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

std::vector<wchar_t> environment() {
    // Only OS/runtime discovery paths are inherited. No provider, cloud, package
    // registry, Git, proxy, or other credential variables are forwarded.
    constexpr const wchar_t* allowed[] = {L"SystemRoot", L"WINDIR", L"SystemDrive", L"PATH", L"PATHEXT",
        L"TEMP", L"TMP", L"USERPROFILE", L"HOMEDRIVE", L"HOMEPATH", L"APPDATA", L"LOCALAPPDATA", L"PROGRAMDATA",
        L"LANG", L"LC_ALL"};
    struct Environment {
        wchar_t* value = GetEnvironmentStringsW();
        ~Environment() { if (value) FreeEnvironmentStringsW(value); }
    } source;
    if (!source.value) windows_error("GetEnvironmentStringsW");
    std::vector<wchar_t> result;
    for (auto* entry = source.value; *entry; entry += std::wcslen(entry) + 1) {
        const auto* equal = std::wcschr(entry, L'=');
        if (!equal || equal == entry) continue;
        const auto length = static_cast<int>(equal - entry);
        if (std::ranges::any_of(allowed, [&](const wchar_t* name) {
            return CompareStringOrdinal(entry, length, name, -1, TRUE) == CSTR_EQUAL;
        })) result.insert(result.end(), entry, entry + std::wcslen(entry) + 1);
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
            throw std::system_error(static_cast<int>(error), std::system_category(), "InitializeProcThreadAttributeList");
        }
    }
    ~Attributes() {
        if (memory_) { DeleteProcThreadAttributeList(get()); HeapFree(GetProcessHeap(), 0, memory_); }
    }
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory_); }
private:
    void* memory_ = nullptr;
};

void output_pipe(Handle& read, Handle& write) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &security, 65536)) windows_error("CreatePipe");
    read.reset(r); write.reset(w);
    if (!SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0)) windows_error("SetHandleInformation");
}
#else
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

[[noreturn]] void posix_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void pipe_pair(Descriptor& read, Descriptor& write) {
    int pair[2];
    if (pipe(pair) != 0) posix_error("pipe");
    read.reset(pair[0]); write.reset(pair[1]);
    // Keep endpoints away from stdio when the host has a closed standard stream.
    for (auto* descriptor : {&read, &write}) {
        if (descriptor->get() < 3) {
            const auto duplicate = fcntl(descriptor->get(), F_DUPFD_CLOEXEC, 3);
            if (duplicate < 0) posix_error("fcntl(F_DUPFD_CLOEXEC)");
            descriptor->reset(duplicate);
        }
    }
    if (fcntl(read.get(), F_SETFD, FD_CLOEXEC) < 0 || fcntl(write.get(), F_SETFD, FD_CLOEXEC) < 0)
        posix_error("fcntl(FD_CLOEXEC)");
}

void nonblocking(int fd) {
    const auto flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) posix_error("fcntl(O_NONBLOCK)");
}

std::vector<std::string> environment() {
    constexpr std::string_view allowed[] = {"PATH", "HOME", "USER", "LOGNAME", "TMPDIR", "TMP", "TEMP", "LANG", "LC_ALL", "LC_CTYPE"};
    std::vector<std::string> result;
    for (auto** entry = environ; entry && *entry; ++entry) {
        const std::string_view value(*entry);
        const auto name = value.substr(0, value.find('='));
        if (std::ranges::find(allowed, name) != std::end(allowed)) result.emplace_back(value);
    }
    return result;
}

// Pipe writes must not change the host application's process-wide SIGPIPE policy.
class BlockSigpipe {
public:
    BlockSigpipe() {
        sigemptyset(&set_); sigaddset(&set_, SIGPIPE);
        const auto error = pthread_sigmask(SIG_BLOCK, &set_, &original_);
        if (error) throw std::system_error(error, std::generic_category(), "pthread_sigmask");
        sigset_t pending;
        sigpending(&pending);
        pending_before_ = sigismember(&pending, SIGPIPE) == 1;
    }
    ~BlockSigpipe() {
        if (broken && !pending_before_ && sigismember(&original_, SIGPIPE) != 1) {
            sigset_t pending;
            sigpending(&pending);
            if (sigismember(&pending, SIGPIPE) == 1) { int signal = 0; sigwait(&set_, &signal); }
        }
        pthread_sigmask(SIG_SETMASK, &original_, nullptr);
    }
    bool broken = false;
private:
    sigset_t set_{}, original_{};
    bool pending_before_ = false;
};
#endif
} // namespace

struct LineProcess::Impl {
    explicit Impl(std::size_t limit) : limit(limit) {}
    ~Impl() { terminate(); }

    std::size_t limit;
    std::mutex mutex;
    std::condition_variable ready;
    std::string pending;
    std::string failure;
    bool output_closed = false;
    std::atomic_bool stopped{false};
    std::thread reader;
#ifdef _WIN32
    std::unique_ptr<WindowsSandbox> sandbox;
    Handle input, output, errors, job, process;
#else
    Descriptor input, output, errors;
    pid_t pid = -1;
#ifdef __linux__
    std::unique_ptr<LinuxSandbox> sandbox;
#endif
#endif

    void kill_tree() noexcept {
#ifdef _WIN32
        if (job.get()) TerminateJobObject(job.get(), 1);
        else if (process.get()) TerminateProcess(process.get(), 1);
#else
#ifdef __linux__
        if (sandbox) sandbox->terminate();
#endif
        if (pid > 0) { kill(-pid, SIGKILL); kill(pid, SIGKILL); }
#endif
    }

    void terminate() noexcept {
        stopped.store(true);
        kill_tree();
        ready.notify_all();
        if (reader.joinable()) reader.join();
        input.reset(); output.reset(); errors.reset();
#ifdef _WIN32
        if (process.get()) WaitForSingleObject(process.get(), 1000);
        process.reset(); job.reset();
        sandbox.reset();
#else
        if (pid > 0) {
            const auto deadline = Clock::now() + std::chrono::seconds(1);
            for (;;) {
                const auto result = waitpid(pid, nullptr, WNOHANG);
                if (result == pid || (result < 0 && errno == ECHILD)) break;
                if (Clock::now() >= deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // An uninterruptible child can outlive the bounded wait; its group
            // has already received SIGKILL. Never block the agent destructor.
            pid = -1;
        }
#ifdef __linux__
        sandbox.reset();
#endif
#endif
    }

    void check(Deadline deadline, const std::atomic_bool& cancelled) {
        if (cancelled.load()) throw std::runtime_error("RPC request cancelled");
        if (Clock::now() >= deadline) throw std::runtime_error("RPC request timed out");
        std::lock_guard lock(mutex);
        if (!failure.empty()) throw std::runtime_error(failure);
        if (stopped.load()) throw std::runtime_error("RPC process is terminated");
    }

    void append(const char* bytes, std::size_t size) {
        std::lock_guard lock(mutex);
        if (size > limit - pending.size()) throw std::runtime_error("RPC pending output exceeds configured byte limit");
        pending.append(bytes, size);
        ready.notify_all();
    }

    void drain() noexcept {
        try {
            std::array<char, 8192> buffer{};
            bool stdout_closed = false, stderr_closed = false;
            while (!stopped.load() && (!stdout_closed || !stderr_closed)) {
                bool progress = false;
                for (int stream = 0; stream != 2; ++stream) {
                    auto& closed = stream == 0 ? stdout_closed : stderr_closed;
                    for (std::size_t drained = 0; !closed && drained < 65536 && !stopped.load();) {
#ifdef _WIN32
                        const auto handle = stream == 0 ? output.get() : errors.get();
                        DWORD available = 0, count = 0;
                        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
                            if (GetLastError() == ERROR_BROKEN_PIPE) { closed = true; break; }
                            windows_error("PeekNamedPipe(RPC)");
                        }
                        if (!available) break;
                        if (!ReadFile(handle, buffer.data(), std::min<DWORD>(available, buffer.size()), &count, nullptr)) {
                            if (GetLastError() == ERROR_BROKEN_PIPE) { closed = true; break; }
                            windows_error("ReadFile(RPC)");
                        }
                        if (!count) { closed = true; break; }
#else
                        const auto fd = stream == 0 ? output.get() : errors.get();
                        const auto count = read(fd, buffer.data(), buffer.size());
                        if (count == 0) { closed = true; break; }
                        if (count < 0) {
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            posix_error("read(RPC)");
                        }
#endif
                        if (stream == 0) append(buffer.data(), static_cast<std::size_t>(count));
                        // stderr is deliberately discarded, including credentials a server prints.
                        drained += static_cast<std::size_t>(count);
                        progress = true;
                    }
                }
                if (stdout_closed) {
#ifdef __linux__
                    // A protocol server that closes stdout cannot serve further
                    // requests. Stop it immediately, even if stderr stays open.
                    if (sandbox) sandbox->terminate();
#endif
                    std::lock_guard lock(mutex);
                    output_closed = true;
                    ready.notify_all();
                }
                if (progress) std::this_thread::yield();
                else {
#ifdef _WIN32
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
#else
                    std::array<pollfd, 2> descriptors{{{stdout_closed ? -1 : output.get(), POLLIN, 0},
                                                     {stderr_closed ? -1 : errors.get(), POLLIN, 0}}};
                    if (poll(descriptors.data(), descriptors.size(), 10) < 0 && errno != EINTR) posix_error("poll(RPC)");
#endif
                }
            }
        } catch (const std::exception& error) {
            {
                std::lock_guard lock(mutex);
                failure = error.what();
            }
            stopped.store(true);
            kill_tree();
            ready.notify_all();
        }
    }
};

LineProcess::LineProcess(const ProcessRequest& request, const std::atomic_bool* cancelled) {
    const auto check_cancelled=[&] {
        if (cancelled && cancelled->load()) throw std::runtime_error("RPC process startup cancelled");
    };
    check_cancelled();
    validate(request);
    const auto startup_deadline = Clock::now() + request.timeout;
    impl_ = std::make_unique<Impl>(request.max_output_bytes);
#ifdef _WIN32
    if (request.sandbox == SandboxMode::appcontainer) impl_->sandbox = std::make_unique<WindowsSandbox>(request,nullptr,cancelled);
    auto* sandbox = impl_->sandbox.get();
    impl_->job.reset(sandbox ? sandbox->create_job() : CreateJobObjectW(nullptr, nullptr));
    if (!impl_->job.get()) windows_error("CreateJobObjectW(RPC)");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(impl_->job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        windows_error("SetInformationJobObject(RPC)");
    if (sandbox) sandbox->supervise_job(impl_->job.get());

    // A byte-mode, nonblocking server end makes even a child that never reads
    // stdin cancellable without a detached writer or dangling OVERLAPPED memory.
    static std::atomic_uint64_t sequence{0};
    const auto name = L"\\\\.\\pipe\\codestudio-rpc-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(Clock::now().time_since_epoch().count()) + L"-" + std::to_wstring(sequence.fetch_add(1));
    impl_->input.reset(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
    if (impl_->input.get() == INVALID_HANDLE_VALUE) windows_error("CreateNamedPipeW(RPC)");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle child_input(CreateFileW(name.c_str(), GENERIC_READ, 0, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (child_input.get() == INVALID_HANDLE_VALUE) windows_error("CreateFileW(RPC input)");
    if (!ConnectNamedPipe(impl_->input.get(), nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
        windows_error("ConnectNamedPipe(RPC)");
    Handle child_output, child_errors;
    output_pipe(impl_->output, child_output);
    output_pipe(impl_->errors, child_errors);
    HANDLE launch_job=impl_->job.get();
    Attributes attributes(1 + (sandbox ? WindowsSandbox::extra_attribute_count : 1));
    if (sandbox) sandbox->apply_attributes(attributes.get());
    else if (!UpdateProcThreadAttribute(attributes.get(),0,PROC_THREAD_ATTRIBUTE_JOB_LIST,
        &launch_job,sizeof(launch_job),nullptr,nullptr)) windows_error("UpdateProcThreadAttribute(RPC job)");
    std::array<HANDLE, 3> inherited{child_input.get(), child_output.get(), child_errors.get()};
    if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited.data(), sizeof(inherited), nullptr, nullptr)) windows_error("UpdateProcThreadAttribute(RPC)");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input.get();
    startup.StartupInfo.hStdOutput = child_output.get();
    startup.StartupInfo.hStdError = child_errors.get();
    startup.lpAttributeList = attributes.get();
    const auto& executable = sandbox ? sandbox->executable() : request.executable;
    const auto& directory = sandbox ? sandbox->working_directory() : request.working_directory;
    auto command = quote(executable.native());
    const auto& arguments=sandbox && request.package ? sandbox->arguments() : request.arguments;
    for (const auto& argument : arguments) command += L" " + quote(wide(argument));
    if (command.size() >= 32767) throw std::invalid_argument("RPC command line too long");
    auto child_environment = sandbox ? sandbox->environment() : environment();
    if (Clock::now() >= startup_deadline) throw std::runtime_error("RPC process startup timed out");
    PROCESS_INFORMATION info{};
    check_cancelled();
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        child_environment.data(), directory.c_str(), &startup.StartupInfo, &info))
        windows_error("CreateProcessW(RPC)");
    impl_->process.reset(info.hProcess);
    Handle thread(info.hThread);
    if (sandbox) sandbox->verify(impl_->process.get());
    check_cancelled();
    if (Clock::now() >= startup_deadline) throw std::runtime_error("RPC process startup timed out");
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) windows_error("ResumeThread(RPC)");
#else
    std::vector<std::string> arguments{request.executable.string()};
    arguments.insert(arguments.end(), request.arguments.begin(), request.arguments.end());
#ifdef __linux__
    if (request.sandbox == SandboxMode::linux_namespace) impl_->sandbox = std::make_unique<LinuxSandbox>(request,nullptr,cancelled);
    auto* sandbox = impl_->sandbox.get();
    if (sandbox) arguments = sandbox->arguments(request);
#endif
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    auto child_environment = environment();
#ifdef __linux__
    if (sandbox) child_environment = {"PATH=/usr/bin", "LANG=C.UTF-8"};
#endif
    std::vector<char*> envp;
    for (auto& entry : child_environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    Descriptor child_input, child_output, child_errors, launch_read, launch_write;
    pipe_pair(child_input, impl_->input);
    pipe_pair(impl_->output, child_output);
    pipe_pair(impl_->errors, child_errors);
    pipe_pair(launch_read, launch_write);
    nonblocking(impl_->input.get()); nonblocking(impl_->output.get()); nonblocking(impl_->errors.get());
    const auto maximum_fd = sysconf(_SC_OPEN_MAX);
    if (maximum_fd < 0) posix_error("sysconf(_SC_OPEN_MAX)");
    if (Clock::now() >= startup_deadline) throw std::runtime_error("RPC process startup timed out");
    const auto parent_pid = getpid();
    check_cancelled();
    const auto pid = fork();
    if (pid < 0) posix_error("fork(RPC)");
    if (pid == 0) {
        const auto fail = [&](int error) {
            const auto ignored = write(launch_write.get(), &error, sizeof(error));
            (void)ignored;
            _exit(127);
        };
        if (setpgid(0, 0) != 0 || chdir(request.working_directory.c_str()) != 0 ||
            dup2(child_input.get(), STDIN_FILENO) < 0 || dup2(child_output.get(), STDOUT_FILENO) < 0 ||
            dup2(child_errors.get(), STDERR_FILENO) < 0) fail(errno);
#ifdef __linux__
        if (sandbox) {
            if (prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent_pid) fail(ECANCELED);
            if (syscall(SYS_close_range,3U,~0U,CLOSE_RANGE_CLOEXEC)<0) fail(errno);
            if (const auto error=sandbox->child_setup()) fail(error);
        } else
#endif
        for (long fd = 3; fd < maximum_fd; ++fd) if (fd != launch_write.get()) close(static_cast<int>(fd));
        execve(arguments.front().c_str(), argv.data(), envp.data());
        fail(errno);
    }
    impl_->pid = pid;
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) posix_error("setpgid(RPC)");
    child_input.reset(); child_output.reset(); child_errors.reset(); launch_write.reset();
    for (;;) {
        check_cancelled();
        if (Clock::now() >= startup_deadline) throw std::runtime_error("RPC process startup timed out");
        pollfd descriptor{launch_read.get(), POLLIN, 0};
        const auto ready = poll(&descriptor, 1, 10);
        if (ready < 0 && errno != EINTR) posix_error("poll(RPC startup)");
        if (ready <= 0) continue;
        int error = 0;
        const auto count = read(launch_read.get(), &error, sizeof(error));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) posix_error("read(RPC startup)");
        if (count) throw std::system_error(error, std::generic_category(), "execve(RPC)");
        break;
    }
#endif
    impl_->reader = std::thread([state = impl_.get()] { state->drain(); });
}

LineProcess::~LineProcess() = default;

void LineProcess::terminate() noexcept { impl_->terminate(); }

void LineProcess::send(std::string_view line, Deadline deadline, const std::atomic_bool& cancelled) {
    try {
        impl_->check(deadline, cancelled);
        if (line.find_first_of("\r\n") != std::string_view::npos) throw std::invalid_argument("RPC send requires exactly one line");
        valid_utf8(line);
        if (line.size() >= impl_->limit) throw std::invalid_argument("RPC input exceeds configured byte limit");
        std::string bytes(line);
        bytes += '\n';
#ifndef _WIN32
        BlockSigpipe signal_guard;
#endif
        for (std::size_t offset = 0; offset < bytes.size();) {
            impl_->check(deadline, cancelled);
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(impl_->input.get(), bytes.data() + offset,
                           static_cast<DWORD>(std::min<std::size_t>(16384, bytes.size() - offset)), &count, nullptr))
                windows_error("WriteFile(RPC input)");
            offset += count;
            if (!count) std::this_thread::sleep_for(std::chrono::milliseconds(2));
#else
            const auto count = write(impl_->input.get(), bytes.data() + offset, bytes.size() - offset);
            if (count > 0) { offset += static_cast<std::size_t>(count); continue; }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                signal_guard.broken = errno == EPIPE;
                posix_error("write(RPC input)");
            }
            pollfd descriptor{impl_->input.get(), POLLOUT, 0};
            if (poll(&descriptor, 1, 10) < 0 && errno != EINTR) posix_error("poll(RPC input)");
#endif
        }
        impl_->check(deadline, cancelled);
    } catch (...) { impl_->terminate(); throw; }
}

std::string LineProcess::receive(Deadline deadline, const std::atomic_bool& cancelled) {
    try {
        for (;;) {
            impl_->check(deadline, cancelled);
            std::unique_lock lock(impl_->mutex);
            const auto newline = impl_->pending.find('\n');
            if (newline != std::string::npos) {
                auto line = impl_->pending.substr(0, newline);
                impl_->pending.erase(0, newline + 1);
                lock.unlock();
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.find('\r') != std::string::npos) throw std::runtime_error("RPC output contains embedded carriage return");
                valid_utf8(line);
                return line;
            }
            if (impl_->output_closed) throw std::runtime_error(impl_->pending.empty() ?
                "RPC process closed stdout" : "RPC process closed stdout with an incomplete line");
            impl_->ready.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(10)));
        }
    } catch (...) { impl_->terminate(); throw; }
}
} // namespace codestudio
