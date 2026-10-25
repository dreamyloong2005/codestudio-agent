#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <sddl.h>
#elif defined(__linux__)
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Standalone /MT fixture: nothing in the agent runtime is linked into the
// untrusted process being measured.
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
void emit(const Json& value) { std::cout << value.dump() << '\n' << std::flush; }
#ifdef _WIN32
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("UTF-8 conversion failed");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}
fs::path path(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
Json environment_value(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count) return nullptr;
    std::wstring value(count, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), count);
    if (!copied || copied >= count) throw std::runtime_error("Environment query failed");
    value.resize(copied);
    return utf8(value);
}
fs::path executable() {
    std::wstring name(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!count || count == name.size()) throw std::runtime_error("Image path query failed");
    name.resize(count);
    return fs::path(name);
}
DWORD open_error(const fs::path& name, DWORD access, DWORD disposition) {
    Handle file{CreateFileW(name.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr)};
    return file.value == INVALID_HANDLE_VALUE ? GetLastError() : 0;
}
void write(const fs::path& name, const std::string& value) {
    std::ofstream output(name, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    if (!output) throw std::runtime_error("Fixture output write failed");
}
std::vector<unsigned char> token_information(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD bytes = 0;
    GetTokenInformation(token, kind, nullptr, 0, &bytes);
    if (!bytes) throw std::runtime_error("Token information size failed");
    std::vector<unsigned char> data(bytes);
    if (!GetTokenInformation(token, kind, data.data(), bytes, &bytes))
        throw std::runtime_error("Token information query failed");
    return data;
}
std::string sid_text(PSID sid) {
    if (!sid || !IsValidSid(sid)) throw std::runtime_error("Invalid token SID");
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) throw std::runtime_error("SID conversion failed");
    struct LocalGuard { LPWSTR value; ~LocalGuard() { LocalFree(value); } } guard{text};
    return utf8(text);
}
DWORD breakaway_error();
Json network_probe(int port) {
    Json result{{"network_wsa_startup", 0}, {"network_socket_created", false},
        {"network_socket_error", nullptr}, {"network_loopback_attempted", false},
        {"network_loopback_error", nullptr}};
    WSADATA data{};
    const int started = WSAStartup(MAKEWORD(2, 2), &data);
    result["network_wsa_startup"] = started;
    if (started) return result;
    struct WinsockGuard { ~WinsockGuard() { WSACleanup(); } } winsock;
    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        result["network_socket_error"] = WSAGetLastError();
        return result;
    }
    struct SocketGuard { SOCKET value; ~SocketGuard() { closesocket(value); } } socket_guard{socket_handle};
    result["network_socket_created"] = true;
    result["network_socket_error"] = 0;
    if (port <= 0 || port > 65535) return result;
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking)) throw std::runtime_error("Nonblocking socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));
    result["network_loopback_attempted"] = true;
    int error = connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 ? 0 : WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
        fd_set writable, failed;
        FD_ZERO(&writable); FD_ZERO(&failed);
        FD_SET(socket_handle, &writable); FD_SET(socket_handle, &failed);
        timeval timeout{2, 0};
        const auto ready = select(0, nullptr, &writable, &failed, &timeout);
        if (ready == 0) error = WSAETIMEDOUT;
        else if (ready == SOCKET_ERROR) error = WSAGetLastError();
        else {
            int size = sizeof(error);
            if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size))
                error = WSAGetLastError();
        }
    }
    result["network_loopback_error"] = error;
    return result;
}
Json identity(const Json& probe) {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw_token))
        throw std::runtime_error("Process token open failed");
    Handle token{raw_token};
    DWORD appcontainer = 0, bytes = 0;
    if (!GetTokenInformation(token.value, TokenIsAppContainer, &appcontainer, sizeof(appcontainer), &bytes))
        throw std::runtime_error("AppContainer query failed");
    bool lpac = false;
    if (appcontainer) {
        HANDLE raw_impersonation = nullptr;
        if (!DuplicateToken(token.value, SecurityIdentification, &raw_impersonation))
            throw std::runtime_error("Token duplication failed");
        Handle impersonation{raw_impersonation};
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:SYG:SYD:(A;;0x3;;;WD)(A;;0x1;;;S-1-15-2-1)(A;;0x2;;;S-1-15-2-2)",
            SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("LPAC descriptor failed");
        struct DescriptorGuard { PSECURITY_DESCRIPTOR value; ~DescriptorGuard() { LocalFree(value); } } guard{descriptor};
        GENERIC_MAPPING mapping{};
        std::vector<unsigned char> privileges(1024);
        DWORD privilege_bytes = static_cast<DWORD>(privileges.size()), granted = 0;
        BOOL access = FALSE;
        if (!AccessCheck(descriptor, impersonation.value, MAXIMUM_ALLOWED, &mapping,
            reinterpret_cast<PPRIVILEGE_SET>(privileges.data()), &privilege_bytes, &granted, &access))
            throw std::runtime_error("LPAC access check failed");
        lpac = access && granted == 0x2;
    }
    Json result{{"appcontainer", appcontainer != 0}, {"lpac", lpac}, {"pid", GetCurrentProcessId()},
        {"cwd", utf8(fs::current_path().native())}, {"image", utf8(executable().native())},
        {"api_key_present", GetEnvironmentVariableW(L"CODESTUDIO_API_KEY", nullptr, 0) != 0},
        {"secret_present", GetEnvironmentVariableW(L"CODESTUDIO_BUILD_TEST_SECRET", nullptr, 0) != 0}};
    // Only inspect known nonsecret build settings; never dump the environment.
    result["build_environment"] = {{"HOME", environment_value(L"HOME")},
        {"CARGO_HOME", environment_value(L"CARGO_HOME")},
        {"XMAKE_GLOBALDIR", environment_value(L"XMAKE_GLOBALDIR")},
        {"XMAKE_PKG_INSTALLDIR", environment_value(L"XMAKE_PKG_INSTALLDIR")},
        {"XMAKE_PROGRAM_DIR", environment_value(L"XMAKE_PROGRAM_DIR")}};
    const auto app_sid = token_information(token.value, TokenAppContainerSid);
    const auto* app_info = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(app_sid.data());
    result["appcontainer_sid"] = app_info->TokenAppContainer ? Json(sid_text(app_info->TokenAppContainer)) : Json(nullptr);
    const auto capability_data = token_information(token.value, TokenCapabilities);
    const auto* capabilities = reinterpret_cast<const TOKEN_GROUPS*>(capability_data.data());
    result["capabilities"] = Json::array();
    for (DWORD index = 0; index < capabilities->GroupCount; ++index) {
        result["capabilities"].push_back({{"sid", sid_text(capabilities->Groups[index].Sid)},
            {"attributes", capabilities->Groups[index].Attributes}});
    }
    const auto integrity_data = token_information(token.value, TokenIntegrityLevel);
    const auto integrity = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity_data.data())->Label.Sid;
    if (!IsValidSid(integrity) || !*GetSidSubAuthorityCount(integrity)) throw std::runtime_error("Invalid integrity SID");
    result["integrity_rid"] = *GetSidSubAuthority(integrity, *GetSidSubAuthorityCount(integrity) - 1);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &job, sizeof(job), nullptr)) {
        result["job_query_error"] = 0;
        result["job_active_process_limit"] = job.BasicLimitInformation.ActiveProcessLimit;
        result["job_limit_flags"] = job.BasicLimitInformation.LimitFlags;
        result["job_memory_bytes"] = job.JobMemoryLimit;
    } else result["job_query_error"] = GetLastError();
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    if (QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu), nullptr)) {
        result["job_cpu_query_error"] = 0;
        result["job_cpu_flags"] = cpu.ControlFlags;
        result["job_cpu_rate"] = cpu.CpuRate;
    } else result["job_cpu_query_error"] = GetLastError();
    result["breakaway_error"] = breakaway_error();
    result.update(network_probe(probe.value("loopback_port", 0)));
    // Keep a kernel-device probe independent of Winsock initialization or
    // socket creation so each denial stage is reported precisely.
    result["network_afd_open"] = open_error(L"\\\\?\\GLOBALROOT\\Device\\Afd", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    result["mount_create"] = open_error(executable().parent_path() / L"must-not-create.txt", GENERIC_WRITE, CREATE_NEW);
    if (probe.contains("outside_file")) {
        const auto outside = path(probe.at("outside_file").get<std::string>());
        result["outside_read"] = open_error(outside, GENERIC_READ, OPEN_EXISTING);
        result["outside_write"] = open_error(outside, GENERIC_WRITE, OPEN_EXISTING);
        result["outside_create"] = open_error(path(probe.at("outside_new").get<std::string>()), GENERIC_WRITE, CREATE_NEW);
    }
    if (probe.contains("staged_file")) {
        const auto attributes = GetFileAttributesW(path(probe.at("staged_file").get<std::string>()).c_str());
        result["staged_file_exists"] = attributes != INVALID_FILE_ATTRIBUTES;
        result["staged_file_error"] = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    }
    return result;
}
// Windows argv quoting, including JSON quotes and trailing backslashes.
std::wstring quoted(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
DWORD breakaway_error() {
    const auto image = executable();
    auto command = quoted(image.native()) + L" breakaway-probe";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB,
        nullptr, nullptr, &startup, &process)) return GetLastError();
    // A regression must never leave the unexpectedly permitted process running.
    Handle process_handle{process.hProcess}, thread_handle{process.hThread};
    if (!TerminateProcess(process.hProcess, 1) || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
        throw std::runtime_error("Unable to reclaim unexpected breakaway probe");
    return 0;
}
int spawn(const std::wstring& mode, int generation, const std::wstring& probe) {
    const auto image = executable();
    auto command = quoted(image.native()) + L" " + quoted(mode) + L" " +
        std::to_wstring(generation) + L" " + quoted(probe);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) throw std::runtime_error("Descendant launch failed: " + std::to_string(GetLastError()));
    Handle process_handle{process.hProcess}, thread_handle{process.hThread};
    if (WaitForSingleObject(process.hProcess, 35000) != WAIT_OBJECT_0)
        throw std::runtime_error("Descendant wait failed");
    DWORD status = 0;
    if (!GetExitCodeProcess(process.hProcess, &status)) throw std::runtime_error("Descendant status failed");
    return static_cast<int>(status);
}
int execute(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"write";
    const bool descendant = mode == L"child" || mode == L"child-sleep";
    const int generation = descendant ? std::stoi(argv[2]) : 0;
    const std::wstring probe_text = argc > (descendant ? 3 : 2) ? argv[descendant ? 3 : 2] : L"{}";
    auto probe = Json::parse(utf8(probe_text));
    // Expanded Windows paths are separate argv values; embedding them in a
    // JSON argument would require a second layer of JSON escaping.
    if (!descendant && argc > 3) probe["staged_file"] = utf8(argv[3]);
    fs::create_directories(L"out");
    auto evidence = identity(probe);
    evidence["generation"] = generation;
    write(fs::path(L"out") / (L"identity-" + std::to_wstring(generation) + L".json"), evidence.dump());
    emit(evidence);
    if (mode == L"fail") { write(L"out/failed.txt", "must never be exported"); return 17; }
    if (mode == L"tree" || mode == L"tree-sleep" || descendant) {
        const bool sleeping = mode == L"tree-sleep" || mode == L"child-sleep";
        if (generation < 2) {
            const auto status = spawn(sleeping ? L"child-sleep" : L"child", generation + 1, probe_text);
            if (status) return status;
        }
        if (sleeping) Sleep(30000);
        return 0;
    }
    if (mode == L"sleep") { Sleep(30000); return 0; }
    if (mode != L"write") return 2;
    std::ifstream input(fs::path(L"input.txt"), std::ios::binary);
    if (!input) throw std::runtime_error("Staged source missing");
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    write(L"out/payload.txt", content + "\nprivate build output\n");
    write(L"input.txt", "changed only in the private workspace");
    return 0;
}
#elif defined(__linux__)
std::string read(const fs::path& name) {
    std::ifstream input(name, std::ios::binary);
    if (!input) throw std::runtime_error("Fixture input read failed: " + name.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
void write(const fs::path& name, const std::string& value) {
    std::ofstream output(name, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size())); output.close();
    if (!output) throw std::runtime_error("Fixture output write failed");
}
std::string link_name(const fs::path& name) {
    std::array<char, 16384> buffer{};
    const auto count = readlink(name.c_str(), buffer.data(), buffer.size());
    if (count <= 0 || static_cast<std::size_t>(count) == buffer.size()) throw std::runtime_error("Fixture readlink failed");
    return {buffer.data(), static_cast<std::size_t>(count)};
}
Json environment_value(const char* name) {
    const auto* value = std::getenv(name); return value ? Json(value) : Json(nullptr);
}
int open_error(const fs::path& path, int flags) {
    const auto fd = open(path.c_str(), flags | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    close(fd); return 0;
}
Json identity(const Json& probe) {
    const auto image = fs::path(link_name("/proc/self/exe"));
    const auto uid_map = read("/proc/self/uid_map");
    unsigned long long inside = 0, outside = 0, count = 0;
    std::istringstream mapping(uid_map); mapping >> inside >> outside >> count;
    const auto no_new_privileges = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    Json result{{"linux_namespace", count == 1 && no_new_privileges == 1}, {"uid_map", uid_map},
        {"pid", getpid()}, {"cwd", fs::current_path().string()}, {"image", image.string()},
        {"no_new_privileges", no_new_privileges}, {"seccomp_mode", prctl(PR_GET_SECCOMP, 0, 0, 0, 0)},
        {"api_key_present", std::getenv("CODESTUDIO_API_KEY") != nullptr},
        {"secret_present", std::getenv("CODESTUDIO_BUILD_TEST_SECRET") != nullptr}};
    result["namespaces"] = Json::object();
    for (const auto* name : {"user", "mnt", "pid", "net"})
        result["namespaces"][name] = link_name(fs::path("/proc/self/ns") / name);
    result["build_environment"] = Json::object();
    for (const auto* name : {"HOME", "CARGO_HOME", "XMAKE_GLOBALDIR", "XMAKE_PKG_INSTALLDIR", "XMAKE_PROGRAM_DIR"})
        result["build_environment"][name] = environment_value(name);
    result["mount_create"] = open_error(image.parent_path() / "must-not-create.txt", O_WRONLY | O_CREAT | O_EXCL);
    if (probe.contains("outside_file")) {
        const auto path = fs::path(probe.at("outside_file").get<std::string>());
        result["outside_read"] = open_error(path, O_RDONLY);
        result["outside_write"] = open_error(path, O_WRONLY);
        result["outside_create"] = open_error(fs::path(probe.at("outside_new").get<std::string>()), O_WRONLY | O_CREAT | O_EXCL);
    }
    if (probe.contains("staged_file")) result["staged_file_exists"] = fs::exists(probe.at("staged_file").get<std::string>());
    const auto socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    result["network_socket_created"] = socket_fd >= 0;
    result["network_socket_error"] = socket_fd < 0 ? errno : 0;
    result["network_loopback_attempted"] = false; result["network_loopback_error"] = nullptr;
    if (socket_fd >= 0) {
        const auto port = probe.value("loopback_port", 0);
        if (port > 0 && port <= 65535) {
            timeval timeout{1, 0}; setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<unsigned short>(port));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            result["network_loopback_attempted"] = true;
            result["network_loopback_error"] = connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 ? 0 : errno;
        }
        close(socket_fd);
    }
    return result;
}
int spawn(const std::string& mode, int generation, const std::string& probe) {
    const auto image = link_name("/proc/self/exe"), index = std::to_string(generation);
    const auto child = fork();
    if (child < 0) throw std::runtime_error("Descendant fork failed");
    if (child == 0) {
        execl(image.c_str(), image.c_str(), mode.c_str(), index.c_str(), probe.c_str(), static_cast<char*>(nullptr));
        _exit(125);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) throw std::runtime_error("Descendant wait failed");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
int execute(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "write";
    const bool descendant = mode == "child" || mode == "child-sleep";
    if (descendant && argc < 3) throw std::runtime_error("Missing generation");
    const auto generation = descendant ? std::stoi(argv[2]) : 0;
    const std::string probe_text = argc > (descendant ? 3 : 2) ? argv[descendant ? 3 : 2] : "{}";
    auto probe = Json::parse(probe_text);
    if (!descendant && argc > 3) probe["staged_file"] = argv[3];
    fs::create_directories("out");
    auto evidence = identity(probe); evidence["generation"] = generation;
    write(fs::path("out") / ("identity-" + std::to_string(generation) + ".json"), evidence.dump()); emit(evidence);
    if (mode == "fail") { write("out/failed.txt", "must never be exported"); return 17; }
    if (mode == "prepare") return 0;
    if (mode == "tree" || mode == "tree-sleep" || descendant) {
        const auto sleeping = mode == "tree-sleep" || mode == "child-sleep";
        if (generation < 2) { const auto status = spawn(sleeping ? "child-sleep" : "child", generation + 1, probe_text); if (status) return status; }
        if (sleeping) sleep(30);
        return 0;
    }
    if (mode == "sleep") { sleep(30); return 0; }
    if (mode != "write") return 2;
    const auto content = read("input.txt");
    write("out/payload.txt", content + "\nprivate build output\n");
    write("input.txt", "changed only in the private workspace");
    return 0;
}
#endif
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try { return execute(argc, argv); }
    catch (const std::exception& error) { emit({{"fixture_error", error.what()}}); return 3; }
}
#elif defined(__linux__)
int main(int argc, char** argv) {
    try { return execute(argc, argv); }
    catch (const std::exception& error) { emit({{"fixture_error", error.what()}}); return 3; }
}
#else
int main() { return 0; }
#endif
