#include "codestudio/line_process.hpp"
#include "codestudio/process_package.hpp"
#include "codestudio/digest.hpp"
#include "codestudio/build_workspace.hpp"
#ifdef __linux__
#include "../src/platform/linux_sandbox.hpp"
#endif
#include <algorithm>
#include <array>
#include <fstream>
#include <cstring>
#include <random>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <set>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <userenv.h>
#include <objbase.h>
#endif

namespace {
using namespace std::chrono_literals;
using codestudio::Json;
using Clock = std::chrono::steady_clock;
void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("sandbox test: " + message);
}
template <typename F> void rejects(F&& operation, const char* message) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string(); return {value.begin(), value.end()};
}
void process_package_tests(const std::filesystem::path& fixture) {
    namespace fs=std::filesystem;
    using codestudio::ProcessPackage;
    // Windows TEMP may contain an 8.3 ancestor. The production copier rejects
    // aliases, so resolve the existing fixture parent before creating our tree.
    const auto root=fs::canonical(fs::temp_directory_path())/
        ("codestudio-package-interface-"+std::to_string(std::random_device{}()));
    check(fs::create_directory(root),"create package interface workspace");
    struct Cleanup {fs::path root;~Cleanup(){
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
        try {codestudio::safe_remove_owned_tree(root);} catch(...) {}
#else
        std::error_code error; fs::remove_all(root, error);
#endif
    }} cleanup{root};
    const auto source=root/"source";
    fs::create_directories(source/"data");
    const auto write=[](const fs::path& path,const std::string& text) {
        std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(text.data(),static_cast<std::streamsize>(text.size()));
        check(static_cast<bool>(file),"write package interface fixture");
    };
    const auto record=[](const char* name,const std::string& bytes,bool executable) {
        return Json{{"path",name},{"sha256",codestudio::sha256_bytes(bytes)},
                    {"size",bytes.size()},{"executable",executable}};
    };
    const auto fresh=[&](const char* name) {auto path=root/name;check(fs::create_directory(path),"create empty stage");return path;};
    const std::string executable_bytes="owned-image-bytes",configuration="frozen configuration\n";
    write(source/"entry.exe",executable_bytes);write(source/"data/config.txt",configuration);
    write(source/"unlisted-secret.txt","must-not-be-imported");
#ifndef _WIN32
    fs::permissions(source/"entry.exe",fs::perms::owner_read|fs::perms::owner_write|fs::perms::owner_exec);
#endif
    const Json original=Json::array({record("entry.exe",executable_bytes,true),record("data/config.txt",configuration,false)});
    Json caller_files=original;
    std::vector<std::string> caller_arguments{"--config","${package}/data/config.txt","literal"};
    auto caller_source=source,caller_entry=fs::path("entry.exe");
    const ProcessPackage plan(caller_source,caller_files,caller_entry,caller_arguments);
    caller_files=Json::object();caller_arguments.assign(1,"changed");caller_source.clear();caller_entry.clear();
    const auto good=fresh("good");std::atomic_bool cancelled{false};
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    plan.stage(good,cancelled);
    check(plan.source()==source && plan.entrypoint()=="entry.exe","package owns caller paths");
    const auto mapped=plan.arguments(good);
    check(mapped.size()==3 && mapped[0]=="--config" && mapped[2]=="literal" &&
          fs::path(std::u8string(mapped[1].begin(),mapped[1].end()))==good/"data/config.txt",
          "package owns and maps caller argument templates");
    check(codestudio::read_build_manifest_snapshot(good/"entry.exe")==executable_bytes &&
          codestudio::read_build_manifest_snapshot(good/"data/config.txt")==configuration,
          "package owns exact caller file inventory");
    check(!fs::exists(good/"unlisted-secret.txt"),"only declared payload enters package staging");
    rejects([&]{plan.stage(good,cancelled);},"stage must not overwrite occupied directory");
    const auto stopped=fresh("cancelled");cancelled=true;
    rejects([&]{plan.stage(stopped,cancelled);},"cancelled stage must fail");
    check(fs::is_empty(stopped),"cancelled stage must not copy a file");cancelled=false;

    write(source/"data/config.txt",std::string(configuration.size(),'x'));
    rejects([&]{plan.stage(fresh("hash-mismatch"),cancelled);},"same-size changed bytes must fail digest check");
    write(source/"data/config.txt",configuration+"extra");
    rejects([&]{plan.stage(fresh("size-mismatch"),cancelled);},"changed size must fail frozen package check");
    write(source/"data/config.txt",configuration);
#ifndef _WIN32
    fs::permissions(source/"entry.exe",fs::perms::owner_read|fs::perms::owner_write);
    rejects([&]{plan.stage(fresh("mode-mismatch"),cancelled);},"removed executable mode must fail frozen package check");
    fs::permissions(source/"entry.exe",fs::perms::owner_read|fs::perms::owner_write|fs::perms::owner_exec);
    fs::permissions(source/"data/config.txt",fs::perms::owner_read|fs::perms::owner_write|fs::perms::owner_exec);
    rejects([&]{plan.stage(fresh("data-mode-mismatch"),cancelled);},"added executable mode must fail frozen package check");
    fs::permissions(source/"data/config.txt",fs::perms::owner_read|fs::perms::owner_write);
#endif
    const auto saved=source/"saved-config.txt";
    fs::rename(source/"data/config.txt",saved);
    fs::create_hard_link(saved,source/"data/config.txt");
    rejects([&]{plan.stage(fresh("hardlink"),cancelled);},"package copy must reject hard-linked source");
    fs::remove(source/"data/config.txt");fs::rename(saved,source/"data/config.txt");
#else
    rejects([&]{plan.stage(good,cancelled);},"unsupported package staging must fail closed");
    check(fs::is_empty(good),"unsupported package staging must not copy any files");
#endif
    for(const auto* name:{"../escape","/absolute","data/../config.txt","data//config.txt","data\\config.txt","NUL.txt","data/trailing."}) {
        auto files=original;files[1]["path"]=name;
        rejects([&]{ProcessPackage invalid(source,files,"entry.exe",{});},(std::string("noncanonical package inventory path must fail: ")+name).c_str());
    }
    for(const auto* name:{"../escape","data//config.txt","data\\config.txt"}) {
        const std::vector<std::string> args{std::string("${package}/")+name};
        rejects([&]{ProcessPackage invalid(source,original,"entry.exe",args);},(std::string("noncanonical package argument path must fail: ")+name).c_str());
    }
    auto collision=original;collision.push_back(record("ENTRY.exe",executable_bytes,true));
    rejects([&]{ProcessPackage invalid(source,collision,"entry.exe",{});},"case-folded package filenames must not collide");
    collision=original;collision.push_back(record("DATA/other.txt","other",false));
    rejects([&]{ProcessPackage invalid(source,collision,"entry.exe",{});},"case-folded package directory aliases must not coexist");
    collision=original;collision.push_back(record("DATA","other",false));
    rejects([&]{ProcessPackage invalid(source,collision,"entry.exe",{});},"package file must not collide with a directory");

    const auto reject_message=[&](auto&& operation,const char* expected) {
        try {operation();} catch(const std::exception& error) {
            check(std::string(error.what()).find(expected)!=std::string::npos,
                  std::string("package refusal must come from preflight, got: ")+error.what());return;
        }
        check(false,"invalid package process request was accepted");
    };
    codestudio::ProcessRequest request;
    request.package=std::make_shared<ProcessPackage>(plan);
    request.executable=source/"entry.exe";
    request.arguments=plan.arguments(source);request.working_directory=root;
    reject_message([&]{codestudio::run_process(request,cancelled);},"requires explicit OS isolation");
    reject_message([&]{codestudio::LineProcess process(request);},"requires explicit OS isolation");
#ifdef __linux__
    request.sandbox=codestudio::SandboxMode::linux_namespace;
#else
    request.sandbox=codestudio::SandboxMode::appcontainer;
#endif
    codestudio::validate_package_limits(request);
    request.arguments.push_back("unapproved");
    reject_message([&]{codestudio::validate_package_limits(request);},"differs from frozen plan");
    request.arguments=plan.arguments(source);request.executable=fixture;
    reject_message([&]{codestudio::validate_package_limits(request);},"differs from frozen plan");
    // Exercise both public launch paths when isolation is actually available;
    // the exact preflight error must occur before an image can be started.
#if defined(_WIN32) || defined(__linux__)
    const bool launch_preflight_available=
#ifdef _WIN32
        true;
#else
        std::getenv("CODESTUDIO_CGROUP_ROOT")!=nullptr;
#endif
    if(launch_preflight_available) {
        reject_message([&]{codestudio::run_process(request,cancelled);},"differs from frozen plan");
        reject_message([&]{codestudio::LineProcess process(request);},"differs from frozen plan");
    }
#endif
    request.executable=source/"entry.exe";request.package_limits.processes=33;
    reject_message([&]{codestudio::validate_package_limits(request);},"Invalid process package resource limits");
    request.package_limits={};request.package_limits.memory_bytes=1;
    reject_message([&]{codestudio::validate_package_limits(request);},"Invalid process package resource limits");
    request.package_limits={};request.package_limits.cpu_percent=0;
    reject_message([&]{codestudio::validate_package_limits(request);},"Invalid process package resource limits");
}
#ifdef _WIN32
struct Listener {
    WSADATA startup{};
    SOCKET socket = INVALID_SOCKET;
    unsigned short port = 0;
    Listener() {
        check(WSAStartup(MAKEWORD(2, 2), &startup) == 0, "start host Winsock");
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        check(socket != INVALID_SOCKET, "create host listener");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind host loopback");
        check(listen(socket, SOMAXCONN) == 0, "listen on host loopback");
        int size = sizeof(address);
        check(getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0, "get host port");
        port = ntohs(address.sin_port);
    }
    ~Listener() { if (socket != INVALID_SOCKET) closesocket(socket); WSACleanup(); }
};
struct Environment {
    std::wstring original;
    bool present = false;
    Environment() {
        const auto size = GetEnvironmentVariableW(L"CODESTUDIO_SANDBOX_TEST_SECRET", nullptr, 0);
        present = size != 0;
        if (present) {
            original.resize(size);
            original.resize(GetEnvironmentVariableW(L"CODESTUDIO_SANDBOX_TEST_SECRET", original.data(), size));
        }
        check(SetEnvironmentVariableW(L"CODESTUDIO_SANDBOX_TEST_SECRET", L"test-secret-must-not-reach-child"), "set environment sentinel");
    }
    ~Environment() {
        SetEnvironmentVariableW(L"CODESTUDIO_SANDBOX_TEST_SECRET", present ? original.c_str() : nullptr);
    }
};
void identity(const Json& report) {
    check(report.at("appcontainer") == true && report.at("lpac") == true, "actual child token must be LPAC: " + report.dump());
    check(report.at("job_query_error") == 0 && report.at("job_active_process_limit") == 1,
        "actual child job must enforce a single active process: " + report.dump());
    const auto flags = report.at("job_limit_flags").get<DWORD>();
    check((flags & (JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)) ==
        (JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE), "job must enforce active-process limit and kill-on-close");
    check((flags & (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) == 0,
        "job must not permit child-process breakaway");
}
void isolated(const Json& report, const std::filesystem::path& workspace) {
    identity(report);
    for (const auto* operation : {"outside_read", "outside_write", "outside_create"})
        check(report.at(operation) == ERROR_ACCESS_DENIED, std::string(operation) + " must be denied by the OS: " + report.dump());
    check(report.at("scratch_write") == 0 && report.at("scratch_read") == true, "private scratch must be usable: " + report.dump());
    if (report.at("winsock_startup") == 0) {
        check(report.at("network_connect_attempted") == true, "initialized Winsock must attempt connections");
        check(report.at("network_loopback") == WSAEACCES && report.at("network_external") == WSAEACCES,
            "network connections require an OS access denial, not a timeout/refusal: " + report.dump());
    } else {
        check(report.at("winsock_startup") == WSASYSCALLFAILURE, "unexpected Winsock initialization failure: " + report.dump());
        check(report.at("network_connect_attempted") == false && !report.contains("network_loopback") && !report.contains("network_external"),
            "initialization failure must not be reported as a connect result");
        check(report.at("network_afd_open") == ERROR_ACCESS_DENIED,
            "initialization failure alone is insufficient: kernel network-device access must be denied: " + report.dump());
    }
    check(report.at("secret_present") == false, "arbitrary credentials must not be inherited");
    for (const auto* operation : {"image_write", "bin_create", "root_create"})
        check(report.at(operation) == ERROR_ACCESS_DENIED, std::string(operation) + " must be denied: " + report.dump());
    check(report.at("cwd") != utf8(workspace), "sandbox must not run in original workspace");
}
std::filesystem::path cwd(const Json& report) {
    const auto text = report.at("cwd").get<std::string>();
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::filesystem::path profile_root(const Json& report) {
    const auto name = report.at("profile_name").get<std::string>();
    constexpr std::string_view prefix = "codestudio-sandbox-";
    check(name.starts_with(prefix) && name.size() == prefix.size() + 32 &&
        std::ranges::all_of(std::string_view(name).substr(prefix.size()), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }), "profile name must be a unique sandbox identifier");
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    check(size != 0, "host LOCALAPPDATA must be available");
    std::wstring local(size, L'\0');
    local.resize(GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), size));
    check(!local.empty(), "read host LOCALAPPDATA");
    return std::filesystem::path(local) / L"Packages" / std::wstring(name.begin(), name.end());
}
void profile_present(const Json& report) {
    const auto sid = report.at("appcontainer_sid").get<std::string>();
    const std::wstring wide_sid(sid.begin(), sid.end());
    PWSTR folder = nullptr;
    const auto status = GetAppContainerFolderPath(wide_sid.c_str(), &folder);
    check(SUCCEEDED(status) && folder, "live profile must be registered");
    const auto actual = std::filesystem::path(folder).parent_path();
    CoTaskMemFree(folder);
    check(std::filesystem::equivalent(actual, profile_root(report)), "registered profile must match the owned unique profile path");
}
void profile_removed(const Json& report) {
    check(!std::filesystem::exists(profile_root(report)), "owned AppContainer profile directory must be removed");
    const auto sid = report.at("appcontainer_sid").get<std::string>();
    const std::wstring wide_sid(sid.begin(), sid.end());
    PWSTR folder = nullptr;
    const auto status = GetAppContainerFolderPath(wide_sid.c_str(), &folder);
    if (folder) CoTaskMemFree(folder);
    check(status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "owned AppContainer profile registration must be removed: " + std::to_string(status));
}
void dead(const Json& report) {
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, report.at("pid").get<DWORD>());
    if (!process) { check(GetLastError() == ERROR_INVALID_PARAMETER, "query terminated fixture"); return; }
    const auto state = WaitForSingleObject(process, 1000);
    CloseHandle(process);
    check(state == WAIT_OBJECT_0, "sandbox process must be terminated");
}
void no_inherited_event(const Json& report, HANDLE host_event) {
    const auto kernel = GetModuleHandleW(L"kernelbase.dll");
    check(kernel != nullptr, "kernelbase must be loaded for object identity inspection");
    using CompareHandles = BOOL (WINAPI*)(HANDLE, HANDLE);
    const auto compare = reinterpret_cast<CompareHandles>(GetProcAddress(kernel, "CompareObjectHandles"));
    check(compare != nullptr, "CompareObjectHandles must be available for inherited object inspection");
    const auto process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, report.at("pid").get<DWORD>());
    check(process != nullptr, "open live child to inspect inherited objects");
    HANDLE duplicate = nullptr;
    // Inspect from the host: unknown numeric handles in LPAC can trigger an
    // invalid-handle exception rather than returning a normal Win32 error.
    const auto success = DuplicateHandle(process, host_event, GetCurrentProcess(), &duplicate,
        0, FALSE, DUPLICATE_SAME_ACCESS);
    const auto error = success ? 0 : GetLastError();
    CloseHandle(process);
    if (!success) {
        check(error == ERROR_INVALID_HANDLE, "unexpected inherited-handle inspection error: " + std::to_string(error));
        return;
    }
    const auto same = compare(host_event, duplicate);
    CloseHandle(duplicate);
    check(!same, "extra inheritable host event must not reach child");
}
std::vector<unsigned char> acl(const std::filesystem::path& file) {
    constexpr auto fields = DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION;
    DWORD size = 0;
    GetFileSecurityW(file.c_str(), fields, nullptr, 0, &size);
    check(size != 0, "read security descriptor size");
    std::vector<unsigned char> data(size);
    check(GetFileSecurityW(file.c_str(), fields, data.data(), size, &size), "read original security descriptor");
    return data;
}
void junction(const std::filesystem::path& link, const std::filesystem::path& target) {
    check(CreateDirectoryW(link.c_str(), nullptr), "create cleanup junction directory");
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "open cleanup junction");
    struct Header {
        DWORD tag; USHORT length, reserved, substitute_offset, substitute_length, print_offset, print_length;
    };
    static_assert(sizeof(Header) == 16);
    const auto substitute = L"\\??\\" + target.native();
    const auto print = target.native();
    const auto path_bytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<unsigned char> data(sizeof(Header) + path_bytes);
    const Header header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<USHORT>(path_bytes + 8), 0, 0,
        static_cast<USHORT>(substitute.size() * sizeof(wchar_t)),
        static_cast<USHORT>((substitute.size() + 1) * sizeof(wchar_t)),
        static_cast<USHORT>(print.size() * sizeof(wchar_t))};
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + sizeof(header), substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(data.data() + sizeof(header) + header.print_offset, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    DWORD returned = 0;
    const auto success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data.data(), static_cast<DWORD>(data.size()),
        nullptr, 0, &returned, nullptr);
    const auto error = success ? 0 : GetLastError();
    CloseHandle(handle);
    check(success, "create cleanup junction: " + std::to_string(error));
}
#endif
}

void run_sandbox_tests(const std::filesystem::path& fixture_path) {
    std::atomic_bool cancelled{false};
    const auto fixture = std::filesystem::absolute(fixture_path);
    process_package_tests(fixture);
    codestudio::ProcessRequest request{fixture, {"identity"}, fixture.parent_path(), 5s, 65536,
        codestudio::SandboxMode::appcontainer};
#if !defined(_WIN32) && !defined(__linux__)
    rejects([&] { codestudio::run_process(request, cancelled); }, "unsupported OS must reject sandbox request");
    rejects([&] { codestudio::LineProcess process(request); }, "unsupported OS must reject persistent sandbox request");
#elif defined(__linux__)
    rejects([&] { codestudio::run_process(request, cancelled); }, "Linux must reject Windows sandbox");
    request.sandbox = codestudio::SandboxMode::linux_namespace;
    // A full Linux isolation run is explicit: dependency absence must not be
    // mistaken for a passing isolation test in a lightweight developer run.
    const auto* delegated = std::getenv("CODESTUDIO_CGROUP_ROOT");
    if (!delegated) {
        rejects([&] { codestudio::validate_sandbox_mode(request.sandbox); },
                "undelegated Linux sandbox must fail closed");
        return;
    }
    namespace fs = std::filesystem;
    const fs::path group_parent(delegated);
    const auto groups = [&] {
        std::set<fs::path> result;
        for (const auto& entry:fs::directory_iterator(group_parent))
            if (entry.is_directory() && entry.path().filename().string().starts_with("codestudio-isolation-")) result.insert(entry.path());
        return result;
    };
    const auto baseline = groups();
    const auto assert_clean = [&] {check(groups()==baseline,"sandbox cgroup leaked after normal teardown");};
    const auto root = fs::temp_directory_path() / ("codestudio-linux-sandbox-tests-" +
        std::to_string(std::random_device{}()));
    check(fs::create_directory(root),"create Linux sandbox test workspace");
    struct Cleanup {fs::path root; ~Cleanup(){std::error_code ignored;fs::remove_all(root,ignored);}} cleanup{root};
    const auto outside=root/"host-only.txt", created=root/"must-not-create.txt";
    {std::ofstream output(outside);output<<"host-only-sentinel";}
    const auto descriptor=open(outside.c_str(),O_RDONLY);
    check(descriptor>=0,"open inheritable host sentinel");
    struct FdCleanup {int fd;~FdCleanup(){close(fd);}} fd_cleanup{descriptor};
    const auto listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
    check(listener>=0,"open control socket");FdCleanup listener_cleanup{listener};
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof address)==0 && listen(listener,8)==0,"listen on control endpoint");
    socklen_t address_size=sizeof address;
    check(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&address_size)==0,"get control port");
    const Json parameters{{"outside_file",outside.string()},{"outside_new",created.string()},
        {"port",ntohs(address.sin_port)},{"host_fd",descriptor}};
    const auto* secret=std::getenv("CODESTUDIO_SANDBOX_TEST_SECRET");
    const std::string saved_secret=secret?secret:"";const bool had_secret=secret!=nullptr;
    struct EnvCleanup {bool present;std::string value;~EnvCleanup(){if(present)setenv("CODESTUDIO_SANDBOX_TEST_SECRET",value.c_str(),1);else unsetenv("CODESTUDIO_SANDBOX_TEST_SECRET");}} env_cleanup{had_secret,saved_secret};
    check(setenv("CODESTUDIO_SANDBOX_TEST_SECRET","private-host-secret",1)==0,"set host environment sentinel");
    request.working_directory=root;request.arguments={"probe",parameters.dump()};
    request.sandbox=codestudio::SandboxMode::none;
    auto result=codestudio::run_process(request,cancelled);
    check(result.exit_code==0,"unisolated probe executes: "+result.output);
    auto report=Json::parse(result.output);
    check(report.at("outside_read")==0 && report.at("outside_write")==0 && report.at("outside_create")==0 &&
          report.at("network_loopback")==0 && report.at("secret_present")==true && report.at("inherited_fd_open")==true,
          "unisolated controls actually reach sentinel resources: "+report.dump());
    fs::remove(created);
    const auto identity=[&](const Json& value){
        check(value.at("seccomp")==2 && value.at("no_new_privileges")==1,"kernel seccomp and privilege boundary active: "+value.dump());
        check(value.at("cwd")=="/work" && value.at("home")=="/work" && value.at("pid").get<int>()<=3,
              "private mount and PID namespaces active: "+value.dump());
        check(value.at("secret_present")==false,"host environment secret leaked");
    };
    const auto isolated=[&](const Json& value){
        identity(value);
        for(const auto* name:{"outside_read","outside_write","outside_create","host_etc_read","tool_write"})
            check(value.at(name)!=0,std::string("sandbox permitted ")+name);
        check(value.at("scratch_write")==0,"private workspace not writable");
        check(value.at("network_socket")==EPERM,"network socket syscall not denied");
        check(value.at("inherited_fd_open")==false,"host file descriptor inherited into isolated child");
    };
    request.sandbox=codestudio::SandboxMode::linux_namespace;
    request.arguments={"identity"};
    result=codestudio::run_process(request,cancelled);
    check(result.exit_code==0 && !result.timed_out,
          "private ELF runtime must resolve libstdc++ without host loader cache or library symlinks: "+result.output);
    identity(Json::parse(result.output));assert_clean();
    {
        codestudio::LinuxBuildLimits limits;
        codestudio::LinuxSandbox staging(request,&limits);
        const auto fifo=staging.writable_root()/"fifo-image";
        const auto alias=staging.writable_root()/"alias-image";
        check(mkfifo(fifo.c_str(),0600)==0,"create executable FIFO adversary");
        fs::create_symlink(fixture,alias);
        for(const auto& candidate:{fifo,alias}) {
            const auto reader=fork();check(reader>=0,"fork bounded executable metadata reader");
            if(reader==0) {
                try {staging.configure_build(candidate,staging.writable_root(),{});_exit(1);}
                catch(const std::exception&){_exit(0);}
            }
            const auto deadline=Clock::now()+1s;
            int status=0;bool complete=false;
            while(Clock::now()<deadline) {
                const auto waited=waitpid(reader,&status,WNOHANG);
                if(waited==reader){complete=true;break;}
                check(waited==0 || errno==EINTR,"wait for executable metadata reader");
                std::this_thread::sleep_for(5ms);
            }
            if(!complete){kill(reader,SIGKILL);while(waitpid(reader,&status,0)<0 && errno==EINTR){}}
            check(complete && WIFEXITED(status) && WEXITSTATUS(status)==0,
                  "staged executable metadata must reject FIFO/symlink without blocking host: "+candidate.filename().string());
        }
    }
    assert_clean();
    for(const auto* transport:{"process","lines"}) {
        int feedback[2];check(pipe2(feedback,O_CLOEXEC)==0,"create closed-stdio test feedback");
        const auto test_host=fork();check(test_host>=0,"fork closed-stdio test host");
        if(test_host==0) {
            close(feedback[0]);close(STDIN_FILENO);close(STDOUT_FILENO);close(STDERR_FILENO);
            std::string answer="OK";
            try {
                if(std::string_view(transport)=="process") {
                    const auto outcome=codestudio::run_process(request,cancelled);
                    if(outcome.exit_code!=0)throw std::runtime_error(outcome.output);
                    identity(Json::parse(outcome.output));
                } else {
                    codestudio::LineProcess child(request);
                    identity(Json::parse(child.receive(Clock::now()+5s,cancelled)));
                }
            } catch(const std::exception& error) {answer=error.what();}
            const auto ignored=write(feedback[1],answer.data(),answer.size());(void)ignored;
            _exit(answer=="OK"?0:1);
        }
        close(feedback[1]);FdCleanup feedback_cleanup{feedback[0]};
        pollfd descriptor_poll{feedback[0],POLLIN,0};
        const auto feedback_ready=poll(&descriptor_poll,1,10000);
        if(feedback_ready<=0)kill(test_host,SIGKILL);
        std::array<char,2048> answer{};
        const auto answer_size=feedback_ready>0?read(feedback[0],answer.data(),answer.size()):0;
        int status=0;while(waitpid(test_host,&status,0)<0)check(errno==EINTR,"reap closed-stdio test host");
        check(answer_size==2 && std::string_view(answer.data(),2)=="OK" && WIFEXITED(status) && WEXITSTATUS(status)==0,
              std::string("isolated ")+transport+" must work with host stdio closed: "+std::string(answer.data(),std::max<ssize_t>(0,answer_size)));
        assert_clean();
    }
    request.arguments={"probe",parameters.dump()};
    result=codestudio::run_process(request,cancelled);
    check(result.exit_code==0 && !result.timed_out,"Linux sandbox executes: "+result.output);
    isolated(Json::parse(result.output));assert_clean();
    check(!fs::exists(created),"isolated process created a host file");
    request.arguments={"spawn"};result=codestudio::run_process(request,cancelled);
    check(result.exit_code==5 && Json::parse(result.output).at("spawn_error")==EPERM,"single-process sandbox permitted fork: "+result.output);assert_clean();
    request.arguments={"namespace"};result=codestudio::run_process(request,cancelled);
    check(result.exit_code==0 && Json::parse(result.output).at("unshare_error")==EPERM,"sandbox permitted new namespaces: "+result.output);assert_clean();
    request.arguments={"lines"};
    {
        codestudio::LineProcess child(request);
        for(int i=0;i<2;++i){child.send(parameters.dump(),Clock::now()+5s,cancelled);isolated(Json::parse(child.receive(Clock::now()+5s,cancelled)));}
        const auto active=groups();check(active.size()==baseline.size()+1,"persistent sandbox does not own exactly one cgroup");
        for(const auto& group:active) if(!baseline.contains(group)) {
            const auto read=[&](const char* name){std::ifstream input(group/name);std::string value;std::getline(input,value);return value;};
            check(read("pids.max")=="3" && read("memory.max")=="536870912" && read("cpu.max")=="80000 100000",
                  "single process resource limits missing from kernel cgroup");
        }
    }
    assert_clean();
    request.arguments={"stdout-eof"};
    {
        codestudio::LineProcess child(request);
        identity(Json::parse(child.receive(Clock::now()+5s,cancelled)));
        rejects([&]{(void)child.receive(Clock::now()+2s,cancelled);},"RPC stdout EOF did not terminate transport");
    }
    assert_clean();
    request.arguments={"sleep"};request.timeout=1s;
    result=codestudio::run_process(request,cancelled);
    check(result.timed_out && !result.output.empty(),"Linux isolation timeout did not terminate a running child");identity(Json::parse(result.output));assert_clean();
    request.timeout=5s;
    {std::jthread cancel([&]{std::this_thread::sleep_for(1s);cancelled=true;});result=codestudio::run_process(request,cancelled);}
    check(result.cancelled && !result.timed_out,"Linux isolation cancellation not enforced");cancelled=false;assert_clean();
    {codestudio::LineProcess child(request);identity(Json::parse(child.receive(Clock::now()+5s,cancelled)));}
    assert_clean();
    // Crash the host while an MCP-style transport is alive. Destructors cannot
    // run here; PDEATHSIG and Bubblewrap must still kill the isolated process.
    int readiness[2];check(pipe2(readiness,O_CLOEXEC)==0,"create crash test readiness pipe");
    const auto host=fork();check(host>=0,"fork crashing transport host");
    if(host==0) {
        close(readiness[0]);
        try {
            codestudio::LineProcess child(request);
            (void)child.receive(Clock::now()+5s,cancelled);
            const char ready='R';if(write(readiness[1],&ready,1)!=1)_exit(4);
            for(;;) pause();
        } catch(...) {_exit(3);}
    }
    close(readiness[1]);FdCleanup ready_cleanup{readiness[0]};
    struct CrashHostGuard {
        pid_t pid;
        ~CrashHostGuard(){if(pid>0){kill(pid,SIGKILL);while(waitpid(pid,nullptr,0)<0 && errno==EINTR){}}}
    } crash_guard{host};
    pollfd ready_poll{readiness[0],POLLIN,0};
    check(poll(&ready_poll,1,5000)>0,"crash fixture startup did not become ready");
    char ready=0;check(read(readiness[0],&ready,1)==1 && ready=='R',"crash fixture failed before sandbox startup");
    const auto crashed_groups=groups();check(crashed_groups.size()==baseline.size()+1,"crash test must own exactly one cgroup");
    check(kill(host,SIGKILL)==0,"kill transport host");
    while(waitpid(host,nullptr,0)<0)check(errno==EINTR,"reap crashed transport host");
    crash_guard.pid=-1;
    for(const auto& group:crashed_groups) if(!baseline.contains(group)) {
        bool empty=false;
        const auto deadline=Clock::now()+5s;
        while(Clock::now()<deadline) {
            std::ifstream input(group/"cgroup.events");std::string events((std::istreambuf_iterator<char>(input)),{});
            if(events.find("populated 0")!=std::string::npos){empty=true;break;}
            std::this_thread::sleep_for(10ms);
        }
        check(empty,"isolated MCP process survived abrupt host exit");
        // A crashed host cannot remove its directories; remove only the exact
        // empty owned group and identically named private tree from this test.
        check(fs::remove(group),"remove empty crash-test cgroup");
        codestudio::safe_remove_owned_tree(fs::temp_directory_path()/group.filename());
    }
    assert_clean();
    {std::ifstream input(outside);std::string sentinel;std::getline(input,sentinel);check(sentinel=="host-only-sentinel","host sentinel modified");}
#else
    const auto root = std::filesystem::temp_directory_path() / ("codestudio-sandbox-tests-" +
        std::to_string(std::random_device{}()) + "-" + std::to_string(Clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(root), "create unique test workspace");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{root};
    const auto outside = root / "host-only.txt", created = root / "must-not-create.txt";
    { std::ofstream file(outside); file << "host-only-sentinel"; }
    const auto original_image_acl = acl(fixture), original_root_acl = acl(root), original_outside_acl = acl(outside);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    const auto host_handle = CreateEventW(&attributes, TRUE, FALSE, nullptr);
    check(host_handle != nullptr, "create inheritable host sentinel event");
    struct HandleCleanup { HANDLE value; ~HandleCleanup() { CloseHandle(value); } } handle_cleanup{host_handle};
    DWORD host_flags = 0;
    check(GetHandleInformation(host_handle, &host_flags) && (host_flags & HANDLE_FLAG_INHERIT), "host sentinel event must be inheritable");
    Listener listener;
    Environment environment;
    const Json parameters{{"outside_file", utf8(outside)}, {"outside_new", utf8(created)}, {"port", listener.port}};
    request.working_directory = root;
    request.arguments = {"probe", parameters.dump()};
    // The same probe succeeds without isolation, so denied paths and the local
    // network endpoint cannot accidentally be nonexistent or malformed fixtures.
    request.sandbox = codestudio::SandboxMode::none;
    auto result = codestudio::run_process(request, cancelled);
    check(result.exit_code == 0, "unisolated control executes: " + result.output);
    auto report = Json::parse(result.output);
    check(report.at("appcontainer") == false, "control token is not AppContainer");
    check(report.at("outside_read") == 0 && report.at("outside_write") == 0 && report.at("outside_create") == 0,
        "host control can access all test paths");
    check(report.at("network_loopback") == 0, "host control can connect to the listening loopback endpoint");
    check(report.at("network_afd_open") == 0, "host control can open the kernel network device");
    check(std::filesystem::remove(created), "remove control-created sentinel");
    request.arguments = {"spawn"};
    result = codestudio::run_process(request, cancelled);
    check(result.exit_code == 0 && !result.timed_out, "unisolated direct-child control executes: " + result.output);
    report = Json::parse(result.output);
    check(report.at("appcontainer") == false && report.at("descendant") == true,
        "control must create a real direct descendant using the same fixture path");

    request.sandbox = codestudio::SandboxMode::appcontainer;
    request.arguments = {"identity"};
    result = codestudio::run_process(request, cancelled);
    check(result.exit_code == 0 && !result.timed_out, "sandbox identity executes: exit=" + std::to_string(result.exit_code) +
        ", timeout=" + std::to_string(result.timed_out) + ", output=" + result.output);
    const auto identity_report = Json::parse(result.output);
    identity(identity_report);
    profile_removed(identity_report);
    request.arguments = {"probe", parameters.dump()};
    result = codestudio::run_process(request, cancelled);
    check(result.exit_code == 0 && !result.timed_out, "sandboxed probe executes: exit=" + std::to_string(result.exit_code) +
        ", timeout=" + std::to_string(result.timed_out) + ", output=" + result.output);
    report = Json::parse(result.output);
    isolated(report, root);
    check(!std::filesystem::exists(created), "outside creation produced no effect");
    check(!std::filesystem::exists(cwd(report)), "one-shot scratch removed after child exit");
    profile_removed(report);
    { std::ifstream file(outside); std::string content; std::getline(file, content);
      check(content == "host-only-sentinel", "outside sentinel remains unchanged"); }

    request.arguments = {"lines"};
    std::filesystem::path persistent_scratch;
    std::filesystem::path persistent_profile;
    Json persistent_identity;
    {
        codestudio::LineProcess child(request);
        for (int index = 0; index < 2; ++index) {
            child.send(parameters.dump(), Clock::now() + 5s, cancelled);
            const auto reply = Json::parse(child.receive(Clock::now() + 5s, cancelled));
            isolated(reply, root);
            no_inherited_event(reply, host_handle);
            if (index == 0) {
                profile_present(reply);
                persistent_scratch = cwd(reply); persistent_profile = profile_root(reply); persistent_identity = reply;
            }
            else check(cwd(reply) == persistent_scratch, "persistent child retains its private scratch");
        }
        check(std::filesystem::exists(persistent_scratch), "scratch remains while child is alive");
        // Inject a real directory junction as the host to exercise teardown even
        // on systems where the restricted token cannot create reparse points.
        junction(persistent_scratch / L"outside-junction", root);
        check(std::filesystem::exists(persistent_profile), "profile exists while the child is alive");
        junction(persistent_profile / L"outside-junction", root);
    }
    check(!std::filesystem::exists(persistent_scratch), "persistent scratch removed after termination");
    profile_removed(persistent_identity);
    check(std::filesystem::exists(outside), "cleanup must never follow a junction into the original workspace");

    request.timeout = 5s;
    for (const auto* mode : {"spawn", "breakaway"}) {
        request.arguments = {mode};
        const auto expected_error = std::string_view(mode) == "spawn" ? ERROR_NOT_ENOUGH_QUOTA : ERROR_ACCESS_DENIED;
        result = codestudio::run_process(request, cancelled);
        check(result.exit_code == 5 && !result.timed_out && !result.cancelled,
            "isolated child-process creation must fail immediately: " + result.output);
        report = Json::parse(result.output); identity(report);
        check(report.at("spawn_mode") == mode && report.at("spawn_error") == expected_error,
            "the OS must deny child-process creation with the expected boundary error: " + report.dump());
        check(!std::filesystem::exists(cwd(report)), "child-process denial scratch removed");
        profile_removed(report);
        {
            codestudio::LineProcess child(request);
            report = Json::parse(child.receive(Clock::now() + 5s, cancelled));
            identity(report);
            check(report.at("spawn_mode") == mode && report.at("spawn_error") == expected_error,
                "persistent transport must enforce the same child-process denial: " + report.dump());
        }
        check(!std::filesystem::exists(cwd(report)), "persistent child-process denial scratch removed");
        profile_removed(report);
    }

    request.arguments = {"sleep"};
    request.timeout = 1500ms;
    result = codestudio::run_process(request, cancelled);
    check(result.timed_out, "isolated process timeout must stop execution: " + result.output);
    report = Json::parse(result.output); identity(report);
    check(report.at("sleeping") == true, "sandboxed process actually reached its wait");
    dead(report);
    check(!std::filesystem::exists(cwd(report)), "timed-out process scratch removed");
    profile_removed(report);

    request.timeout = 5s;
    {
        std::jthread cancel([&] { std::this_thread::sleep_for(1500ms); cancelled.store(true); });
        result = codestudio::run_process(request, cancelled);
        cancel.join();
    }
    check(result.cancelled && !result.timed_out, "cancellation terminates the isolated process: " + result.output);
    cancelled.store(false);
    report = Json::parse(result.output); identity(report); dead(report);
    check(!std::filesystem::exists(cwd(report)), "cancelled process scratch removed");
    profile_removed(report);

    {
        codestudio::LineProcess child(request);
        report = Json::parse(child.receive(Clock::now() + 5s, cancelled));
        identity(report);
        check(report.at("sleeping") == true, "persistent sandbox process reached its wait");
    }
    dead(report);
    check(!std::filesystem::exists(cwd(report)), "persistent process scratch removed");
    profile_removed(report);
    check(acl(fixture) == original_image_acl && acl(root) == original_root_acl && acl(outside) == original_outside_acl,
        "sandbox must not change the original executable, workspace or file ACLs");
#endif
}
