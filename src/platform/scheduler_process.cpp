#include "codestudio/scheduler_process.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <map>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <limits>

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
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace codestudio {
#ifdef _WIN32
namespace {
class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

[[noreturn]] void windows_error(const char* operation, DWORD code = GetLastError()) {
    throw std::system_error(static_cast<int>(code), std::system_category(), operation);
}

std::wstring wide(std::string_view text) {
    if (text.empty() || text.size() >= 32767 || text.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid scheduler argument length or NUL");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (!size) windows_error("Invalid scheduler argument encoding");
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size))
        windows_error("Scheduler argument conversion");
    return result;
}

std::wstring current_executable() {
    std::vector<wchar_t> path(512);
    for (;;) {
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size) windows_error("GetModuleFileNameW(scheduler)");
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

class HandleList {
public:
    explicit HandleList(HANDLE inherited) : inherited_(inherited) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        memory_ = HeapAlloc(GetProcessHeap(), 0, size);
        if (!memory_) throw std::bad_alloc();
        if (!InitializeProcThreadAttributeList(get(), 1, 0, &size)) {
            const auto error = GetLastError();
            HeapFree(GetProcessHeap(), 0, memory_);
            memory_ = nullptr;
            windows_error("InitializeProcThreadAttributeList(scheduler)", error);
        }
        if (!UpdateProcThreadAttribute(get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       &inherited_, sizeof(inherited_), nullptr, nullptr)) {
            const auto error = GetLastError();
            DeleteProcThreadAttributeList(get());
            HeapFree(GetProcessHeap(), 0, memory_);
            memory_ = nullptr;
            windows_error("UpdateProcThreadAttribute(scheduler)", error);
        }
    }
    ~HandleList() {
        if (memory_) {
            DeleteProcThreadAttributeList(get());
            HeapFree(GetProcessHeap(), 0, memory_);
        }
    }
    HandleList(const HandleList&) = delete;
    HandleList& operator=(const HandleList&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const {
        return static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory_);
    }
private:
    HANDLE inherited_;
    void* memory_ = nullptr;
};
}
#endif

std::uint32_t start_scheduler_process(const std::vector<std::string>& argv) {
    const bool supervised=argv.size()>1 && argv[1]=="scheduler-supervise";
    std::vector<std::string_view> allowed{"--workspace","--state","--owner","--poll-ms"};
    if (supervised) allowed.insert(allowed.end(),{"--max-restarts","--restart-delay-ms",
        "--expected-stop-epoch","--expected-generation"});
    if (argv.size()!=2+allowed.size()*2 || (!supervised && argv[1]!="scheduler-run"))
        throw std::invalid_argument("Background launch requires a fixed scheduler command");
    if (supervised) for (std::size_t index=2;index<argv.size();index+=2) {
        const auto& key=argv[index];
        if (key!="--max-restarts" && key!="--restart-delay-ms" &&
            key!="--expected-stop-epoch" && key!="--expected-generation") continue;
        const auto& value=argv[index+1]; std::int64_t number=0;
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
        const std::int64_t low=key=="--restart-delay-ms" ? 100 : 0;
        const std::int64_t high=key=="--max-restarts" ? 100 : key=="--restart-delay-ms" ? 60000 :
            key=="--expected-generation" ? INT64_MAX-1 : INT64_MAX;
        if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || number<low || number>high)
            throw std::invalid_argument("Invalid scheduler supervision limit or fence");
    }
#ifdef _WIN32
    std::map<std::string, std::string> options;
    for (std::size_t index = 2; index < argv.size(); index += 2) {
        bool known = false;
        for (const auto key : allowed) if (argv[index] == key) known = true;
        if (!known || !options.emplace(argv[index], argv[index + 1]).second)
            throw std::invalid_argument("Unknown or repeated scheduler launch option");
        (void)wide(argv[index + 1]);
    }
    const auto workspace = wide(options.at("--workspace"));
    const auto state = wide(options.at("--state"));
    if (!std::filesystem::path(workspace).is_absolute() || !std::filesystem::path(state).is_absolute())
        throw std::invalid_argument("Scheduler workspace and state must be absolute paths");
    const auto& owner = options.at("--owner");
    if (owner.size() > 128) throw std::invalid_argument("Scheduler owner token exceeds its limit");
    for (const char ch : owner)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
            throw std::invalid_argument("Invalid scheduler owner token");
    const auto& poll = options.at("--poll-ms");
    unsigned int poll_ms = 0;
    const auto parsed = std::from_chars(poll.data(), poll.data() + poll.size(), poll_ms);
    if (parsed.ec != std::errc{} || parsed.ptr != poll.data() + poll.size() || poll_ms < 20 || poll_ms > 5000)
        throw std::invalid_argument("Scheduler poll-ms must be in 20..5000");

    const auto executable = current_executable();
    std::wstring command = quote(executable) + L" " + wide(argv[1]);
    for (const auto key : allowed)
        command += L" " + wide(key) + L" " + quote(wide(options.at(std::string(key))));
    if (command.size() >= 32767) throw std::invalid_argument("Scheduler command line exceeds its limit");

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle null_io(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (null_io.get() == INVALID_HANDLE_VALUE) windows_error("CreateFileW(scheduler NUL)");
    HandleList inherited(null_io.get());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_io.get();
    startup.StartupInfo.hStdOutput = null_io.get();
    startup.StartupInfo.hStdError = null_io.get();
    startup.lpAttributeList = inherited.get();
    BOOL parent_in_job = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &parent_in_job))
        windows_error("IsProcessInJob(scheduler parent)");
    constexpr DWORD base_flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT;
    const auto launch = [&](DWORD extra, PROCESS_INFORMATION& process) {
        auto mutable_command = command;
        // The explicit handle list contains only NUL, never the client's pipes
        // or runner locks. A null environment preserves the host's credentials.
        return CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                              base_flags | extra, nullptr, workspace.c_str(), &startup.StartupInfo, &process);
    };
    PROCESS_INFORMATION info{};
    if (!launch(parent_in_job ? CREATE_BREAKAWAY_FROM_JOB : 0, info)) {
        const auto error = GetLastError();
        // SILENT_BREAKAWAY jobs may detach only without the explicit flag.
        // The child remains suspended and must pass the same no-Job check.
        if (!parent_in_job || error != ERROR_ACCESS_DENIED)
            windows_error("CreateProcessW(scheduler)", error);
        if (!launch(0, info)) windows_error("CreateProcessW(scheduler without breakaway)");
    }
    Handle process(info.hProcess), thread(info.hThread);
    try {
        BOOL child_in_job = FALSE;
        if (!IsProcessInJob(process.get(), nullptr, &child_in_job))
            windows_error("IsProcessInJob(scheduler child)");
        if (child_in_job)
            throw std::runtime_error("Cannot detach scheduler from the parent Job; run scheduler-run from an independent terminal");
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
            windows_error("ResumeThread(scheduler)");
    } catch (...) {
        // This handle identifies the child we just created, still suspended.
        // Never reopen a recorded PID or terminate an already-detached worker.
        if (TerminateProcess(process.get(), 2)) WaitForSingleObject(process.get(), 5000);
        throw;
    }
    return static_cast<std::uint32_t>(info.dwProcessId);
#elif defined(__linux__)
    std::map<std::string,std::string> options;
    for (std::size_t index=2;index<argv.size();index+=2) {
        bool known=false;
        for (const auto key:allowed) if (argv[index]==key) known=true;
        const auto& value=argv[index+1];
        if (!known || !options.emplace(argv[index],value).second || value.empty() ||
            value.size()>32768 || value.find('\0')!=std::string::npos)
            throw std::invalid_argument("Invalid scheduler launch option");
    }
    if (!std::filesystem::path(options.at("--workspace")).is_absolute() ||
        !std::filesystem::path(options.at("--state")).is_absolute())
        throw std::invalid_argument("Scheduler workspace and state must be absolute paths");
    const auto& owner=options.at("--owner");
    if (owner.size()>128) throw std::invalid_argument("Scheduler owner token exceeds its limit");
    for (const char ch:owner)
        if (!((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9') || ch=='-' || ch=='_'))
            throw std::invalid_argument("Invalid scheduler owner token");
    unsigned poll_ms=0; const auto& value=options.at("--poll-ms");
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),poll_ms);
    if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || poll_ms<20 || poll_ms>5000)
        throw std::invalid_argument("Scheduler poll-ms must be in 20..5000");
    const auto executable=std::filesystem::canonical("/proc/self/exe").string();
    std::vector<std::string> arguments=argv; arguments[0]=executable;
    std::vector<char*> raw; raw.reserve(arguments.size()+1);
    for (auto& argument:arguments) raw.push_back(argument.data());
    raw.push_back(nullptr);
    const auto directory=options.at("--workspace");
    struct rlimit descriptors{};
    if (getrlimit(RLIMIT_NOFILE,&descriptors)!=0)
        throw std::system_error(errno,std::generic_category(),"Read scheduler descriptor limit");
    auto descriptor_limit=descriptors.rlim_cur==RLIM_INFINITY ? static_cast<rlim_t>(INT_MAX) : descriptors.rlim_cur;
    // A descriptor can remain open above a subsequently lowered resource limit.
    // Inspect before fork, so the fallback still closes all inherited handles.
    for (const auto& entry:std::filesystem::directory_iterator("/proc/self/fd")) {
        const auto name=entry.path().filename().string();
        unsigned descriptor=0;
        const auto parsed_fd=std::from_chars(name.data(),name.data()+name.size(),descriptor);
        if (parsed_fd.ec==std::errc{} && parsed_fd.ptr==name.data()+name.size() && descriptor<INT_MAX &&
            static_cast<rlim_t>(descriptor)+1>descriptor_limit)
            descriptor_limit=static_cast<rlim_t>(descriptor)+1;
    }
    int pipefd[2];
    if (pipe2(pipefd,O_CLOEXEC)!=0) throw std::system_error(errno,std::generic_category(),"Create scheduler launch pipe");
    for (auto& fd:pipefd) if (fd<3) {
        const auto replacement=fcntl(fd,F_DUPFD_CLOEXEC,3);
        if (replacement<0) { const auto error=errno; close(pipefd[0]); close(pipefd[1]);
            throw std::system_error(error,std::generic_category(),"Reserve scheduler launch descriptor"); }
        close(fd); fd=replacement;
    }
    const auto first=fork();
    if (first<0) { const auto error=errno; close(pipefd[0]); close(pipefd[1]);
        throw std::system_error(error,std::generic_category(),"Fork scheduler launcher"); }
    struct Receipt { int kind; int value; };
    if (first==0) {
        close(pipefd[0]);
        // After fork from a threaded service, use only async-signal-safe calls.
        const auto report=[&](const Receipt& receipt) {
            ssize_t count;
            do { count=write(pipefd[1],&receipt,sizeof receipt); } while (count<0 && errno==EINTR);
            return count==sizeof receipt;
        };
        const auto fail=[&](int error) { (void)report({2,error}); _exit(127); };
        if (setsid()<0) fail(errno);
        const auto daemon=fork();
        if (daemon<0) fail(errno);
        if (daemon>0) { if (!report({1,daemon})) _exit(127); _exit(0); }
        struct sigaction action{}; action.sa_handler=SIG_DFL; sigemptyset(&action.sa_mask);
        for (const auto signal:{SIGINT,SIGTERM,SIGHUP,SIGPIPE})
            if (sigaction(signal,&action,nullptr)!=0) fail(errno);
        sigset_t mask; sigemptyset(&mask);
        if (sigprocmask(SIG_SETMASK,&mask,nullptr)!=0 || chdir(directory.c_str())!=0) fail(errno);
        const auto nullfd=open("/dev/null",O_RDWR|O_CLOEXEC);
        if (nullfd<0) fail(errno);
        for (int fd=0;fd<3;++fd)
            if (dup2(nullfd,fd)<0 || fcntl(fd,F_SETFD,0)<0) fail(errno);
        if (nullfd>2) close(nullfd);
        // Mark every inherited descriptor close-on-exec, including run locks and
        // client pipes. The launch receipt remains open until successful exec.
#ifdef SYS_close_range
        if (syscall(SYS_close_range,3U,UINT_MAX,4U)!=0)
#endif
        {
            for (rlim_t fd=3;fd<descriptor_limit && fd<=INT_MAX;++fd) {
                const auto flags=fcntl(static_cast<int>(fd),F_GETFD);
                if (flags<0 && errno!=EBADF) fail(errno);
                if (flags>=0 && fcntl(static_cast<int>(fd),F_SETFD,flags|FD_CLOEXEC)<0) fail(errno);
            }
        }
        execv(executable.c_str(),raw.data()); fail(errno);
    }
    close(pipefd[1]);
    Receipt receipt{}; std::size_t received=0; pid_t daemon=0; int failure=0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (std::chrono::steady_clock::now()<deadline) {
        struct pollfd descriptor{pipefd[0],POLLIN|POLLHUP,0};
        const auto ready=poll(&descriptor,1,100);
        if (ready<0) { if (errno==EINTR) continue; failure=errno; break; }
        if (!ready) continue;
        const auto count=read(pipefd[0],reinterpret_cast<char*>(&receipt)+received,sizeof receipt-received);
        if (count<0) { if (errno==EINTR) continue; failure=errno; break; }
        if (!count) break;
        received+=static_cast<std::size_t>(count);
        if (received==sizeof receipt) {
            if (receipt.kind==1) daemon=receipt.value;
            else if (receipt.kind==2) failure=receipt.value;
            else failure=EPROTO;
            received=0;
        }
    }
    close(pipefd[0]); int status=0; pid_t reaped;
    do { reaped=waitpid(first,&status,0); } while (reaped<0 && errno==EINTR);
    if (reaped<0 && !failure) failure=errno;
    if (reaped==first && (!WIFEXITED(status) || WEXITSTATUS(status)!=0) && !failure) failure=EPROTO;
    if (failure) throw std::system_error(failure,std::generic_category(),"Execute detached scheduler");
    if (daemon<=0 || received || std::chrono::steady_clock::now()>=deadline)
        throw std::runtime_error("Detached scheduler exec was not confirmed; inspect scheduler-status before retrying");
    return static_cast<std::uint32_t>(daemon);
#else
    (void)argv;
    throw std::runtime_error("Background scheduler launch is unsupported on this platform; use foreground scheduler-run");
#endif
}
}
