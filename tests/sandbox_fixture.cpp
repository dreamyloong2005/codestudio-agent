#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#ifdef __linux__
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>
#endif

using Json = nlohmann::json;
namespace {
void emit(const Json& value) { std::cout << value.dump() << '\n' << std::flush; }
#ifdef _WIN32
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("fixture UTF-8 conversion failed");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}
std::filesystem::path path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
Json identity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token))
        throw std::runtime_error("fixture token open failed");
    struct TokenGuard { HANDLE value; ~TokenGuard() { CloseHandle(value); } } token_guard{token};
    DWORD appcontainer = 0, bytes = 0;
    if (!GetTokenInformation(token, TokenIsAppContainer, &appcontainer, sizeof(appcontainer), &bytes))
        throw std::runtime_error("fixture AppContainer query failed");
    bool lpac = false;
    if (appcontainer) {
        HANDLE impersonation = nullptr;
        if (!DuplicateToken(token, SecurityIdentification, &impersonation))
            throw std::runtime_error("fixture LPAC token duplication failed");
        TokenGuard impersonation_guard{impersonation};
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:SYG:SYD:(A;;0x3;;;WD)(A;;0x1;;;S-1-15-2-1)(A;;0x2;;;S-1-15-2-2)", SDDL_REVISION_1,
            &descriptor, nullptr)) throw std::runtime_error("fixture LPAC descriptor failed");
        struct DescriptorGuard { PSECURITY_DESCRIPTOR value; ~DescriptorGuard() { LocalFree(value); } } descriptor_guard{descriptor};
        GENERIC_MAPPING mapping{};
        std::vector<unsigned char> privileges(1024);
        DWORD privilege_bytes = static_cast<DWORD>(privileges.size()), granted = 0;
        BOOL access = FALSE;
        if (!AccessCheck(descriptor, impersonation, MAXIMUM_ALLOWED, &mapping,
            reinterpret_cast<PPRIVILEGE_SET>(privileges.data()), &privilege_bytes, &granted, &access))
            throw std::runtime_error("fixture LPAC access check failed");
        lpac = access && granted == 0x2;
    }
    Json result{{"appcontainer", appcontainer != 0}, {"lpac", lpac}, {"pid", GetCurrentProcessId()},
        {"cwd", utf8(std::filesystem::current_path().native())}};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &job, sizeof(job), nullptr)) {
        result["job_query_error"] = 0;
        result["job_active_process_limit"] = job.BasicLimitInformation.ActiveProcessLimit;
        result["job_limit_flags"] = job.BasicLimitInformation.LimitFlags;
    } else result["job_query_error"] = GetLastError();
    if (appcontainer) {
        DWORD size = 0;
        GetTokenInformation(token, TokenAppContainerSid, nullptr, 0, &size);
        if (!size) throw std::runtime_error("fixture AppContainer SID size query failed");
        std::vector<unsigned char> data(size);
        if (!GetTokenInformation(token, TokenAppContainerSid, data.data(), size, &size))
            throw std::runtime_error("fixture AppContainer SID query failed");
        const auto sid = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(data.data())->TokenAppContainer;
        LPWSTR text = nullptr;
        if (!ConvertSidToStringSidW(sid, &text)) throw std::runtime_error("fixture SID conversion failed");
        result["appcontainer_sid"] = utf8(text);
        LocalFree(text);
        // The host owns profile metadata; a zero-capability LPAC token cannot
        // query the host's AppContainer profile registry.
        result["profile_name"] = utf8(std::filesystem::current_path().parent_path().filename().native());
    }
    return result;
}
DWORD open_error(const std::filesystem::path& file, DWORD access, DWORD disposition) {
    const auto handle = CreateFileW(file.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(handle);
    return ERROR_SUCCESS;
}
int connection_error(const char* address, unsigned short port) {
    const auto socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) return WSAGetLastError();
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking)) {
        const auto error = WSAGetLastError(); closesocket(socket_handle); return error;
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &target.sin_addr) != 1) {
        closesocket(socket_handle); return WSAEINVAL;
    }
    auto error = connect(socket_handle, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0 ? 0 : WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
        fd_set writes, exceptions;
        FD_ZERO(&writes); FD_ZERO(&exceptions);
        FD_SET(socket_handle, &writes); FD_SET(socket_handle, &exceptions);
        timeval timeout{0, 300000};
        const auto ready = select(0, nullptr, &writes, &exceptions, &timeout);
        if (ready > 0) {
            int length = sizeof(error);
            if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length))
                error = WSAGetLastError();
        } else error = ready == 0 ? WSAETIMEDOUT : WSAGetLastError();
    }
    closesocket(socket_handle);
    return error;
}
Json probe(const Json& input) {
    auto result = identity();
    result["outside_read"] = open_error(path(input.at("outside_file")), GENERIC_READ, OPEN_EXISTING);
    result["outside_write"] = open_error(path(input.at("outside_file")), GENERIC_WRITE, OPEN_EXISTING);
    result["outside_create"] = open_error(path(input.at("outside_new")), GENERIC_WRITE, CREATE_NEW);
    const auto scratch = std::filesystem::current_path() / L"scratch-marker.txt";
    auto file = CreateFileW(scratch.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    result["scratch_write"] = file == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!WriteFile(file, "sandbox-scratch", 15, &written, nullptr) || written != 15)
            result["scratch_write"] = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        CloseHandle(file);
    }
    file = CreateFileW(scratch.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    result["scratch_read"] = false;
    if (file != INVALID_HANDLE_VALUE) {
        char data[32]{}; DWORD read = 0;
        result["scratch_read"] = ReadFile(file, data, sizeof(data), &read, nullptr) &&
            std::string(data, read) == "sandbox-scratch";
        CloseHandle(file);
    }
    result["secret_present"] = GetEnvironmentVariableW(L"CODESTUDIO_SANDBOX_TEST_SECRET", nullptr, 0) != 0;
    if (result.at("lpac") == true) {
        std::wstring executable(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!count || count == executable.size()) throw std::runtime_error("fixture image path query failed");
        executable.resize(count);
        const std::filesystem::path image(executable);
        result["image_write"] = open_error(image, GENERIC_WRITE, OPEN_EXISTING);
        result["bin_create"] = open_error(image.parent_path() / L"forbidden.dll", GENERIC_WRITE, CREATE_NEW);
        result["root_create"] = open_error(image.parent_path().parent_path() / L"forbidden.txt", GENERIC_WRITE, CREATE_NEW);
    }
    // Winsock initialization can be denied before connect on zero-capability
    // LPAC tokens. Probe the kernel networking device independently; the host
    // control must open this same path successfully.
    result["network_afd_open"] = open_error(L"\\\\?\\GLOBALROOT\\Device\\Afd", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    WSADATA winsock{};
    const auto started = WSAStartup(MAKEWORD(2, 2), &winsock);
    result["winsock_startup"] = started;
    result["network_connect_attempted"] = started == 0;
    if (!started) {
        result["network_loopback"] = connection_error("127.0.0.1", input.at("port").get<unsigned short>());
        if (result.at("lpac") == true) result["network_external"] = connection_error("192.0.2.1", 9);
        WSACleanup();
    }
    return result;
}
int execute(const std::vector<std::string>& arguments) {
    const auto mode = arguments.size() > 1 ? arguments[1] : "identity";
    if (mode == "identity") { emit(identity()); return 0; }
    if (mode == "probe") { emit(probe(Json::parse(arguments.at(2)))); return 0; }
    if (mode == "lines") {
        std::string line;
        while (std::getline(std::cin, line)) emit(probe(Json::parse(line)));
        return 0;
    }
    if (mode == "child-once") {
        auto result = identity(); result["descendant"] = true; emit(result);
        return 0;
    }
    if (mode == "sleep") {
        auto result = identity(); result["sleeping"] = true; emit(result);
        Sleep(30000);
        return 0;
    }
    if (mode == "spawn" || mode == "breakaway") {
        std::wstring executable(32768, L'\0');
        const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!count || count == executable.size()) return 4;
        executable.resize(count);
        auto command = L"\"" + executable + L"\" child-once";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION info{};
        const auto flags = CREATE_NO_WINDOW | (mode == "breakaway" ? CREATE_BREAKAWAY_FROM_JOB : 0);
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                flags, nullptr, nullptr, &startup, &info)) {
            const auto error = GetLastError();
            auto result = identity(); result["spawn_error"] = error; result["spawn_mode"] = mode; emit(result); return 5;
        }
        CloseHandle(info.hThread); CloseHandle(info.hProcess);
        return 0;
    }
    return 2;
}
#endif
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> arguments;
        for (int index = 0; index < argc; ++index) arguments.push_back(utf8(argv[index]));
        return execute(arguments);
    } catch (const std::exception& error) { emit({{"fixture_error", error.what()}}); return 3; }
}
#elif defined(__linux__)
namespace {
Json linux_identity() {
    return {{"pid",getpid()},{"cwd",std::filesystem::current_path().string()},
        {"seccomp",prctl(PR_GET_SECCOMP)},{"no_new_privileges",prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)},
        {"secret_present",std::getenv("CODESTUDIO_SANDBOX_TEST_SECRET")!=nullptr},
        {"home",std::getenv("HOME")?std::getenv("HOME"):""}};
}
int open_result(const std::string& path,int flags) {
    const auto fd=open(path.c_str(),flags,0600);
    if (fd<0) return errno;
    close(fd); return 0;
}
Json linux_probe(const Json& parameters) {
    auto report=linux_identity();
    report["outside_read"]=open_result(parameters.at("outside_file"),O_RDONLY);
    report["outside_write"]=open_result(parameters.at("outside_file"),O_WRONLY);
    report["outside_create"]=open_result(parameters.at("outside_new"),O_CREAT|O_WRONLY);
    report["scratch_write"]=open_result("scratch.txt",O_CREAT|O_WRONLY);
    report["host_etc_read"]=open_result("/etc/passwd",O_RDONLY);
    report["tool_write"]=open_result("/tools/sandbox-fixture",O_WRONLY);
    report["inherited_fd_open"]=fcntl(parameters.value("host_fd",-1),F_GETFD)>=0;
    const auto connection=socket(AF_INET,SOCK_STREAM,0);
    report["network_socket"]=connection<0?errno:0;
    if (connection>=0) {
        sockaddr_in address{}; address.sin_family=AF_INET;
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(parameters.at("port").get<unsigned short>());
        report["network_loopback"]=connect(connection,reinterpret_cast<sockaddr*>(&address),sizeof address)<0?errno:0;
        close(connection);
    }
    return report;
}
}
int main(int argc,char** argv) {
    try {
        if (argc<2) return 2;
        const std::string mode=argv[1];
        if (mode=="identity") {emit(linux_identity());return 0;}
        if (mode=="probe" && argc==3) {emit(linux_probe(Json::parse(argv[2])));return 0;}
        if (mode=="lines") {
            for (std::string line;std::getline(std::cin,line);) emit(linux_probe(Json::parse(line)));
            return 0;
        }
        if (mode=="spawn") {
            auto report=linux_identity(); const auto child=fork();
            if (child==0) _exit(0);
            report["spawn_error"]=child<0?errno:0;
            if (child>0) waitpid(child,nullptr,0);
            emit(report); return child<0?5:0;
        }
        if (mode=="namespace") {
            auto report=linux_identity(); report["unshare_error"]=unshare(CLONE_NEWUSER)<0?errno:0;
            emit(report); return 0;
        }
        if (mode=="sleep" || mode=="stdout-eof") {
            auto report=linux_identity(); report["sleeping"]=true; emit(report);
            if (mode=="stdout-eof") close(STDOUT_FILENO);
            sleep(30); return 0;
        }
        return 2;
    } catch(const std::exception& error) {emit({{"fixture_error",error.what()}});return 3;}
}
#else
int main() { return 0; }
#endif
