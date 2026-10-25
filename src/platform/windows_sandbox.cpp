#include "windows_sandbox.hpp"
#include "codestudio/build_workspace.hpp"
#include "codestudio/process_package.hpp"

#ifdef _WIN32
#include <sddl.h>
#include <aclapi.h>
#include <userenv.h>
#include <bcrypt.h>
#include <combaseapi.h>
#include <winternl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>

namespace codestudio {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void fail(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct LocalMemory {
    void* value = nullptr;
    ~LocalMemory() { if (value) LocalFree(value); }
};

struct TaskMemory {
    PWSTR value = nullptr;
    ~TaskMemory() { if (value) CoTaskMemFree(value); }
};

struct FindHandle {
    HANDLE value;
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
};

std::vector<unsigned char> token_information(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    if (!size) fail("GetTokenInformation(sandbox size)");
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token, kind, data.data(), size, &size)) fail("GetTokenInformation(sandbox)");
    return data;
}

std::wstring sid_string(PSID sid) {
    LocalMemory memory;
    if (!ConvertSidToStringSidW(sid, reinterpret_cast<LPWSTR*>(&memory.value))) fail("ConvertSidToStringSidW");
    return static_cast<wchar_t*>(memory.value);
}

std::wstring unique_name() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Cannot generate sandbox identity");
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result = L"codestudio-sandbox-";
    for (const auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

// Keep all staging on a local drive and refuse to walk through existing links.
void validate_local_path(const fs::path& path) {
    if (!path.is_absolute() || path.native().starts_with(L"\\\\") ||
        path.native().find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Sandbox paths must be absolute local paths");
    const auto type = GetDriveTypeW(path.root_path().c_str());
    if (type != DRIVE_FIXED && type != DRIVE_RAMDISK)
        throw std::invalid_argument("Sandbox paths must be on a local fixed drive");
    DWORD flags = 0;
    if (!GetVolumeInformationW(path.root_path().c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
        fail("GetVolumeInformationW(sandbox)");
    if (!(flags & FILE_PERSISTENT_ACLS))
        throw std::invalid_argument("Sandbox paths require a filesystem with persistent ACLs");
    fs::path prefix = path.root_path();
    for (const auto& part : path.relative_path()) {
        if (part == L".." || part == L".") throw std::invalid_argument("Sandbox paths must be normalized");
        prefix /= part;
        const auto attributes = GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) fail("GetFileAttributesW(sandbox path)");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::invalid_argument("Sandbox paths cannot traverse reparse points");
    }
}

void create_directory(const fs::path& path, const std::wstring& descriptor) {
    LocalMemory memory;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&memory.value), nullptr)) fail("ConvertStringSecurityDescriptor(sandbox)");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), memory.value, FALSE};
    if (!CreateDirectoryW(path.c_str(), &attributes)) fail("CreateDirectoryW(sandbox)");
}

void verify_directory_acl(const fs::path& path, PSID app_sid, DWORD expected_access) {
    LocalMemory descriptor;
    PACL dacl = nullptr;
    const auto error = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value));
    if (error != ERROR_SUCCESS)
        throw std::system_error(static_cast<int>(error), std::system_category(), "GetNamedSecurityInfoW(sandbox)");
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor.value, &control, &revision)) fail("GetSecurityDescriptorControl(sandbox)");
    if (!(control & SE_DACL_PROTECTED) || !dacl || dacl->AceCount != 3)
        throw std::runtime_error("Sandbox directory did not retain its private protected ACL");
    bool found = false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* entry = nullptr;
        if (!GetAce(dacl, index, &entry)) fail("GetAce(sandbox)");
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE)
            throw std::runtime_error("Sandbox directory has an unexpected ACL entry");
        if (EqualSid(const_cast<DWORD*>(&ace->SidStart), app_sid)) {
            if (found || ace->Mask != expected_access ||
                (ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) !=
                    (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))
                throw std::runtime_error("Sandbox directory has unexpected application permissions");
            found = true;
        }
    }
    if (!found) throw std::runtime_error("Sandbox directory is missing its application ACL");
}

void copy_executable(const fs::path& source, const fs::path& target) {
    // No sharing for writes/deletion while the exact approved image is copied.
    Handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) fail("CreateFileW(sandbox source)");
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(input.value, &information)) fail("GetFileInformationByHandle(sandbox source)");
    if (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        throw std::invalid_argument("Sandbox executable must be a regular non-reparse file");
    const auto size = (static_cast<unsigned long long>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
    if (!size || size > 256ULL * 1024 * 1024)
        throw std::invalid_argument("Sandbox executable must be between 1 byte and 256 MiB");
    Handle output(CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.value == INVALID_HANDLE_VALUE) fail("CreateFileW(sandbox staged image)");
    std::array<unsigned char, 65536> buffer{};
    unsigned long long copied = 0;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) fail("ReadFile(sandbox image)");
        if (!count) break;
        copied += count;
        if (copied > size) throw std::runtime_error("Sandbox executable changed while staging");
        DWORD written = 0;
        if (!WriteFile(output.value, buffer.data(), count, &written, nullptr) || written != count)
            fail("WriteFile(sandbox image)");
    }
    if (copied != size) throw std::runtime_error("Sandbox executable changed while staging");
}

// Hold each directory without FILE_SHARE_DELETE while enumerating it. Reparse
// entries are removed by handle, never traversed. A failed cleanup leaves the
// private tree behind rather than following an untrusted target or widening ACLs.
void remove_entry(const fs::path& path, unsigned depth, std::size_t& budget) {
    if (!budget || depth > 64) return;
    --budget;
    Handle handle(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle.value, &information)) return;
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW data{};
        const auto search = FindFirstFileW((path / L"*").c_str(), &data);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (std::wstring_view(data.cFileName) != L"." && std::wstring_view(data.cFileName) != L"..")
                    remove_entry(path / data.cFileName, depth + 1, budget);
            } while (budget && FindNextFileW(search, &data));
            FindClose(search);
        }
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_READONLY) && information.nNumberOfLinks == 1) {
        FILE_BASIC_INFO basic{};
        basic.FileAttributes = information.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY;
        if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        SetFileInformationByHandle(handle.value, FileBasicInfo, &basic, sizeof(basic));
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof(disposition));
}

// The caller keeps an open, non-reparse directory handle without SHARE_DELETE.
// No untrusted entries may remain for DeleteAppContainerProfile to recurse into.
bool clear_profile_contents(const fs::path& path, HANDLE anchor) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(anchor, &information) ||
        !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    std::size_t budget = 65536;
    WIN32_FIND_DATAW data{};
    {
        FindHandle search{FindFirstFileW((path / L"*").c_str(), &data)};
        if (search.value == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
        do {
            if (std::wstring_view(data.cFileName) != L"." && std::wstring_view(data.cFileName) != L"..")
                remove_entry(path / data.cFileName, 1, budget);
        } while (budget && FindNextFileW(search.value, &data));
        if (!budget || GetLastError() != ERROR_NO_MORE_FILES) return false;
    }
    // Re-enumerate rather than treating best-effort removal as proof of success.
    FindHandle remaining{FindFirstFileW((path / L"*").c_str(), &data)};
    if (remaining.value == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (std::wstring_view(data.cFileName) != L"." && std::wstring_view(data.cFileName) != L"..") return false;
    } while (FindNextFileW(remaining.value, &data));
    return GetLastError() == ERROR_NO_MORE_FILES;
}
}

struct WindowsSandbox::Impl {
    fs::path root, image, work, scratch, profile_root;
    PSID sid = nullptr;
    std::wstring profile_name;
    SECURITY_CAPABILITIES capabilities{};
    DWORD lpac = PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT;
    HANDLE job = nullptr;
    HANDLE profile_anchor = INVALID_HANDLE_VALUE;
    std::vector<wchar_t> environment;
    std::vector<std::string> arguments;
    bool build = false;
    bool package = false;
    bool registry_read = false;
    WindowsBuildLimits build_limits;
    std::vector<unsigned char> registry_sid;
    SID_AND_ATTRIBUTES registry_capability{};
    HANDLE path_alias = nullptr;
    fs::path alias_root;

    ~Impl() {
        bool stopped = true;
        if (job) {
            TerminateJobObject(job, 1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            for (;;) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                stopped = QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                    &accounting, sizeof(accounting), nullptr) && accounting.ActiveProcesses == 0;
                if (stopped || std::chrono::steady_clock::now() >= deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CloseHandle(job);
        }
        if (path_alias) CloseHandle(path_alias);
        if (stopped && !root.empty()) {
            std::size_t budget = build ? 1000000 : 65536;
            try { remove_entry(root, 0, budget); } catch (...) { /* Best-effort cleanup never follows unsafe paths. */ }
        }
        bool profile_empty = false;
        if (stopped && profile_anchor != INVALID_HANDLE_VALUE && !profile_root.empty()) {
            try { profile_empty = clear_profile_contents(profile_root, profile_anchor); } catch (...) {}
        }
        if (profile_anchor != INVALID_HANDLE_VALUE) CloseHandle(profile_anchor);
        // No child remains and no untrusted filesystem content is handed to the
        // profile API. Failed/partial cleanup deliberately preserves the profile.
        if (profile_empty && !profile_name.empty()) DeleteAppContainerProfile(profile_name.c_str());
        if (sid) FreeSid(sid);
    }
};

WindowsSandbox::WindowsSandbox(const ProcessRequest& request, const WindowsBuildLimits* build,
                               const std::atomic_bool* cancelled) : impl_(std::make_unique<Impl>()) {
    if (request.sandbox != SandboxMode::appcontainer) throw std::invalid_argument("WindowsSandbox requires appcontainer mode");
    const std::atomic_bool not_cancelled{false};
    const auto& cancellation = cancelled ? *cancelled : not_cancelled;
    const auto check_cancelled = [&] {
        if (cancellation.load()) throw std::runtime_error("Windows sandbox preparation cancelled");
    };
    check_cancelled();
    if (build && request.package) throw std::invalid_argument("Workspace build and process package cannot share a sandbox request");
    validate_package_limits(request);
    impl_->package = static_cast<bool>(request.package);
    impl_->arguments = request.arguments;
    if (!build) {
        validate_local_path(request.executable);
        if (request.package) validate_local_path(request.package->source());
        auto extension = (request.package ? request.package->entrypoint() : request.executable).extension().native();
        std::ranges::transform(extension, extension.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (extension != L".exe") throw std::invalid_argument("AppContainer sandbox requires an .exe entrypoint");
        if (request.package) {
            impl_->build_limits.processes = request.package_limits.processes;
            impl_->build_limits.memory_bytes = request.package_limits.memory_bytes;
            impl_->build_limits.cpu_percent = request.package_limits.cpu_percent;
        }
    } else {
        if (build->processes < 1 || build->processes > 256 || build->memory_bytes < 64ULL*1024*1024 ||
            build->memory_bytes > 32ULL*1024*1024*1024 || build->cpu_percent < 1 || build->cpu_percent > 100)
            throw std::invalid_argument("Invalid isolated build job limits");
        impl_->build = true;
        impl_->build_limits = *build;
    }

    const auto name = unique_name();
    const auto derived = CreateAppContainerProfile(name.c_str(), L"CodeStudio temporary sandbox", L"CodeStudio isolated child", nullptr, 0, &impl_->sid);
    if (FAILED(derived)) throw std::system_error(static_cast<int>(derived), std::system_category(), "CreateAppContainerProfile");
    impl_->profile_name = name;
    impl_->capabilities.AppContainerSid = impl_->sid;
    impl_->registry_read = build || (request.package && request.package_limits.processes > 1);
    if (impl_->registry_read) {
        // Windows process creation inside LPAC needs the registryRead capability
        // for system compatibility checks. It grants no network capability.
        const auto derive = reinterpret_cast<decltype(&DeriveCapabilitySidsFromName)>(
            GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "DeriveCapabilitySidsFromName"));
        if (!derive) fail("DeriveCapabilitySidsFromName unavailable");
        struct DerivedSids {
            PSID* groups = nullptr; DWORD group_count = 0;
            PSID* capabilities = nullptr; DWORD capability_count = 0;
            ~DerivedSids() {
                for (DWORD i=0;i<group_count;++i) LocalFree(groups[i]);
                for (DWORD i=0;i<capability_count;++i) LocalFree(capabilities[i]);
                LocalFree(groups); LocalFree(capabilities);
            }
        } derived;
        if (!derive(L"registryRead", &derived.groups, &derived.group_count, &derived.capabilities, &derived.capability_count))
            fail("DeriveCapabilitySidsFromName(registryRead)");
        if (derived.capability_count != 1 || !IsValidSid(derived.capabilities[0]))
            throw std::runtime_error("Unexpected registryRead capability identity");
        impl_->registry_sid.resize(GetLengthSid(derived.capabilities[0]));
        if (!CopySid(static_cast<DWORD>(impl_->registry_sid.size()), impl_->registry_sid.data(), derived.capabilities[0]))
            fail("CopySid(registryRead)");
        impl_->registry_capability = {impl_->registry_sid.data(), SE_GROUP_ENABLED};
        impl_->capabilities.Capabilities = &impl_->registry_capability;
        impl_->capabilities.CapabilityCount = 1;
    }

    TaskMemory profile_path;
    const auto profile_status = GetAppContainerFolderPath(sid_string(impl_->sid).c_str(), &profile_path.value);
    if (FAILED(profile_status))
        throw std::system_error(static_cast<int>(profile_status), std::system_category(), "GetAppContainerFolderPath");
    const fs::path app_data(profile_path.value);
    const auto profile_root = app_data.parent_path();
    if (app_data.filename() != L"AC" || profile_root.filename() != name)
        throw std::runtime_error("Windows returned an unexpected sandbox profile directory");
    validate_local_path(profile_root);
    impl_->profile_anchor = CreateFileW(profile_root.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (impl_->profile_anchor == INVALID_HANDLE_VALUE) fail("CreateFileW(sandbox profile anchor)");
    BY_HANDLE_FILE_INFORMATION profile_information{};
    if (!GetFileInformationByHandle(impl_->profile_anchor, &profile_information)) fail("GetFileInformationByHandle(sandbox profile)");
    if (!(profile_information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (profile_information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Sandbox profile root must be a non-reparse directory");
    impl_->profile_root = profile_root;

    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) fail("OpenProcessToken(sandbox owner)");
    Handle token(raw_token);
    const auto user_data = token_information(token.value, TokenUser);
    const auto owner = sid_string(reinterpret_cast<const TOKEN_USER*>(user_data.data())->User.Sid);
    const auto app = sid_string(impl_->sid);
    const auto base_acl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + owner + L")";
    const auto read_acl = base_acl + L"(A;OICI;GRGX;;;" + app + L")";
    const auto write_acl = base_acl + L"(A;OICI;0x001301bf;;;" + app + L")S:(ML;OICI;NW;;;LW)";
    const auto temporary_input = fs::temp_directory_path().lexically_normal();
    validate_local_path(temporary_input);
    std::wstring long_temporary(32768,L'\0');
    const auto long_length = GetLongPathNameW(temporary_input.c_str(),long_temporary.data(),static_cast<DWORD>(long_temporary.size()));
    if (!long_length || long_length >= long_temporary.size()) fail("GetLongPathNameW(sandbox temporary directory)");
    long_temporary.resize(long_length);
    const fs::path temporary(long_temporary);
    validate_local_path(temporary);
    const auto root = temporary / name;
    create_directory(root, read_acl);
    impl_->root = root; // Cleanup owns only a successfully, exclusively created tree.
    verify_directory_acl(root, impl_->sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
    if (build) {
        // LLVM's GetLongPathNameW walks every ancestor, even for long paths.
        // A temporary object-manager alias projects only our private tree;
        // no host ancestor ACLs or existing drive mappings are changed. Without
        // OBJ_PERMANENT/OBJ_INHERIT it disappears when the host handle closes,
        // including abrupt process death.
        using CreateLink = NTSTATUS (NTAPI*)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PUNICODE_STRING);
        const auto create_link = reinterpret_cast<CreateLink>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateSymbolicLinkObject"));
        if (!create_link) fail("NtCreateSymbolicLinkObject unavailable");
        auto target_name = L"\\??\\" + root.native();
        if (target_name.size() > 16000) throw std::invalid_argument("Build private path is too long");
        UNICODE_STRING target{static_cast<USHORT>(target_name.size()*sizeof(wchar_t)),
            static_cast<USHORT>(target_name.size()*sizeof(wchar_t)),target_name.data()};
        // Normal DOS paths also work with tools that join paths with '/'. A
        // named \\.\device alias breaks tools that require ordinary DOS paths.
        // Query both volume and DOS mappings; CREATE without OPENIF atomically
        // refuses collisions. Never replace, remove or reuse an existing link.
        const auto occupied = GetLogicalDrives();
        if (!occupied) fail("GetLogicalDrives(build path mapping)");
        std::array<wchar_t,32768> existing{};
        for (wchar_t letter=L'Z'; letter>=L'D'; --letter) {
            if (occupied & (1UL << (letter-L'A'))) continue;
            const std::wstring drive{letter,L':'};
            if (QueryDosDeviceW(drive.c_str(),existing.data(),static_cast<DWORD>(existing.size())) || GetLastError()!=ERROR_FILE_NOT_FOUND)
                continue;
            auto object_name = L"\\??\\" + drive;
            UNICODE_STRING object{static_cast<USHORT>(object_name.size()*sizeof(wchar_t)),
                static_cast<USHORT>(object_name.size()*sizeof(wchar_t)),object_name.data()};
            OBJECT_ATTRIBUTES attributes{sizeof(attributes),nullptr,&object,OBJ_CASE_INSENSITIVE,nullptr,nullptr};
            HANDLE candidate = nullptr;
            const auto status = create_link(&candidate,0x000F0001,&attributes,&target);
            if (static_cast<ULONG>(status)==0xc0000035UL) continue; // STATUS_OBJECT_NAME_COLLISION
            if (status < 0) throw std::runtime_error("Cannot create temporary private build path mapping (NTSTATUS " + std::to_string(status) + ")");
            impl_->path_alias = candidate;
            impl_->alias_root = fs::path(drive + L"\\");
            break;
        }
        if (!impl_->path_alias) throw std::runtime_error("No unused drive letter is available for the private build path mapping");
    }
    create_directory(root / L"bin", read_acl);
    verify_directory_acl(root / L"bin", impl_->sid, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
    impl_->work = root / L"work";
    impl_->scratch = impl_->work;
    create_directory(impl_->work, write_acl);
    verify_directory_acl(impl_->work, impl_->sid, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE);
    check_cancelled();
    if (request.package) {
        // All payload files inherit this profile's read/execute-only ACL.
        // The immutable plan hashes the copied bytes before any child exists.
        const auto package_root = root / L"bin";
        request.package->stage(package_root, cancellation);
        impl_->image = package_root / request.package->entrypoint();
        impl_->arguments = request.package->arguments(package_root);
    } else if (!build) {
        impl_->image = root / L"bin" / request.executable.filename();
        copy_executable(request.executable, impl_->image);
    }
    check_cancelled();

    std::array<wchar_t, 32768> windows{};
    const auto count = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!count || count >= windows.size()) fail("GetWindowsDirectoryW(sandbox)");
    const fs::path windows_path(std::wstring(windows.data(), count));
    // A deterministic allowlist: never forward the host PATH, credentials,
    // proxy settings, language runtimes, or startup injection variables.
    std::vector<std::wstring> entries{
        L"APPDATA=" + impl_->work.native(), L"HOMEDRIVE=" + impl_->work.root_name().native(),
        L"HOMEPATH=\\" + impl_->work.relative_path().native(), L"LOCALAPPDATA=" + impl_->work.native(),
        L"SystemDrive=" + windows_path.root_name().native(), L"SystemRoot=" + windows_path.native(),
        L"TEMP=" + impl_->work.native(), L"TMP=" + impl_->work.native(),
        L"USERPROFILE=" + impl_->work.native(), L"WINDIR=" + windows_path.native()};
    for (const auto& entry : entries) {
        impl_->environment.insert(impl_->environment.end(), entry.begin(), entry.end());
        impl_->environment.push_back(L'\0');
    }
    impl_->environment.push_back(L'\0');
}

WindowsSandbox::~WindowsSandbox() = default;
const fs::path& WindowsSandbox::executable() const { return impl_->image; }
const fs::path& WindowsSandbox::working_directory() const { return impl_->work; }
const std::vector<std::string>& WindowsSandbox::arguments() const { return impl_->arguments; }
const std::vector<wchar_t>& WindowsSandbox::environment() const { return impl_->environment; }
const fs::path& WindowsSandbox::writable_root() const { return impl_->scratch; }
const fs::path& WindowsSandbox::readonly_root() const { return impl_->root; }
fs::path WindowsSandbox::build_path(const fs::path& physical) const {
    if (!impl_->build || impl_->alias_root.empty()) throw std::logic_error("Build path mapping is not initialized");
    const auto relative = physical.lexically_relative(impl_->root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == L"..")
        throw std::invalid_argument("Build path escapes private storage");
    return impl_->alias_root / relative;
}

void WindowsSandbox::configure_build(const fs::path& executable, const fs::path& directory,
                                    const std::map<std::string, std::string>& supplied) {
    if (!impl_->build) throw std::logic_error("Not a workspace build sandbox");
    validate_local_path(executable);
    validate_local_path(directory);
    // Every path is resolved beneath the private tree, never a host tool path.
    const auto is_inside = [&](const fs::path& path) {
        const auto relative = path.lexically_relative(impl_->root);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != L"..";
    };
    if (!is_inside(executable) || !is_inside(directory)) throw std::invalid_argument("Build step escapes private storage");
    impl_->image = build_path(executable);
    impl_->work = build_path(directory);
    std::map<std::wstring,std::wstring> entries;
    for (const auto* current=impl_->environment.data(); *current; current += std::wcslen(current)+1) {
        const std::wstring entry(current);
        const auto equals = entry.find(L'=');
        entries[entry.substr(0,equals)] = entry.substr(equals+1);
    }
    const auto wide = [](const std::string& input) {
        if (input.empty()) return std::wstring{};
        const int count = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
        if (!count) fail("MultiByteToWideChar(build environment)");
        std::wstring result(count,L'\0');
        if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),result.data(),count))
            fail("MultiByteToWideChar(build environment)");
        return result;
    };
    for (const auto& [key,value] : supplied) entries[wide(key)] = wide(value);
    impl_->environment.clear();
    for (const auto& [key,value] : entries) {
        const auto entry = key + L"=" + value;
        impl_->environment.insert(impl_->environment.end(),entry.begin(),entry.end());
        impl_->environment.push_back(0);
    }
    impl_->environment.push_back(0);
    if (impl_->environment.size() > 32767) throw std::invalid_argument("Build environment exceeds 32767 characters");
}

void WindowsSandbox::apply_attributes(LPPROC_THREAD_ATTRIBUTE_LIST attributes) {
    if (!impl_->job) throw std::logic_error("Sandbox job must be set before process attributes");
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
        &impl_->capabilities, sizeof(impl_->capabilities), nullptr, nullptr)) fail("UpdateProcThreadAttribute(AppContainer)");
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
        &impl_->lpac, sizeof(impl_->lpac), nullptr, nullptr)) fail("UpdateProcThreadAttribute(LPAC)");
    // Atomic assignment closes the suspended-but-unassigned failure window.
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
        &impl_->job, sizeof(impl_->job), nullptr, nullptr)) fail("UpdateProcThreadAttribute(sandbox job)");
}

void WindowsSandbox::supervise_job(HANDLE job) {
    if (impl_->job) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION previous{};
        if (!impl_->build || !QueryInformationJobObject(impl_->job,JobObjectBasicAccountingInformation,&previous,sizeof(previous),nullptr) || previous.ActiveProcesses)
            throw std::logic_error("Sandbox previous process tree has not stopped");
        CloseHandle(impl_->job); impl_->job = nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // Enforce the complete process tree without CHILD_PROCESS_POLICY, which
    // can fail LPAC loader initialization on supported Windows builds.
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = (impl_->build || impl_->package) ? impl_->build_limits.processes : 1;
    if (impl_->build || impl_->package) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = impl_->build_limits.memory_bytes;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        fail("SetInformationJobObject(sandbox process limits)");
    if (impl_->build || impl_->package) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu.CpuRate = impl_->build_limits.cpu_percent * 100;
        if (!SetInformationJobObject(job,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu))) fail("SetInformationJobObject(sandbox CPU cap)");
    }
    if (!DuplicateHandle(GetCurrentProcess(), job, GetCurrentProcess(), &impl_->job, 0, FALSE, DUPLICATE_SAME_ACCESS))
        fail("DuplicateHandle(sandbox job)");
}

void WindowsSandbox::verify(HANDLE process) const {
    if (!impl_->job) throw std::logic_error("Sandbox process must be supervised by a job");
    BOOL belongs = FALSE;
    if (!IsProcessInJob(process, impl_->job, &belongs) || !belongs)
        throw std::runtime_error("Sandbox process was not assigned to its job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr))
        fail("QueryInformationJobObject(sandbox limits)");
    constexpr auto required = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    constexpr auto breakaway = JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if ((limits.BasicLimitInformation.LimitFlags & required) != required ||
        (limits.BasicLimitInformation.LimitFlags & breakaway) ||
        limits.BasicLimitInformation.ActiveProcessLimit != ((impl_->build || impl_->package) ? impl_->build_limits.processes : 1))
        throw std::runtime_error("Sandbox job did not enforce the process limit policy");
    if (impl_->build || impl_->package) {
        if (!(limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) || limits.JobMemoryLimit != impl_->build_limits.memory_bytes)
            throw std::runtime_error("Sandbox job did not enforce the memory limit");
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
        if (!QueryInformationJobObject(impl_->job,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu),nullptr) ||
            cpu.ControlFlags != (JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) ||
            cpu.CpuRate != impl_->build_limits.cpu_percent * 100) throw std::runtime_error("Sandbox job did not enforce the CPU cap");
    }
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY | TOKEN_DUPLICATE, &raw_token)) fail("OpenProcessToken(sandbox child)");
    Handle token(raw_token);
    DWORD appcontainer = 0, size = 0;
    if (!GetTokenInformation(token.value, TokenIsAppContainer, &appcontainer, sizeof(appcontainer), &size))
        fail("GetTokenInformation(AppContainer)");
    if (size != sizeof(appcontainer) || appcontainer != 1)
        throw std::runtime_error("Windows did not create an AppContainer sandbox token");
    // Verify the actual LPAC access-check semantics. TokenIsLessPrivilegedAppContainer
    // is unsupported on some Windows versions that fully support LPAC creation.
    // A normal AC receives both bits; LPAC must ignore ALL APPLICATION PACKAGES
    // and receive only the ALL RESTRICTED APPLICATION PACKAGES bit.
    HANDLE raw_impersonation = nullptr;
    if (!DuplicateToken(token.value, SecurityIdentification, &raw_impersonation)) fail("DuplicateToken(LPAC verification)");
    Handle impersonation(raw_impersonation);
    LocalMemory descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:SYG:SYD:(A;;0x3;;;WD)(A;;0x1;;;S-1-15-2-1)(A;;0x2;;;S-1-15-2-2)", SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr)) fail("ConvertStringSecurityDescriptor(LPAC verification)");
    GENERIC_MAPPING mapping{};
    std::vector<unsigned char> privilege_buffer(1024);
    DWORD privilege_bytes = static_cast<DWORD>(privilege_buffer.size()), granted = 0;
    BOOL access = FALSE;
    if (!AccessCheck(descriptor.value, impersonation.value, MAXIMUM_ALLOWED, &mapping,
        reinterpret_cast<PPRIVILEGE_SET>(privilege_buffer.data()), &privilege_bytes, &granted, &access))
        fail("AccessCheck(LPAC verification)");
    if (!access || granted != 0x2)
        throw std::runtime_error("Windows did not enforce the required LPAC access restrictions");
    const auto app = token_information(token.value, TokenAppContainerSid);
    const auto* app_info = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(app.data());
    if (!app_info->TokenAppContainer || !EqualSid(app_info->TokenAppContainer, impl_->sid))
        throw std::runtime_error("Sandbox process has an unexpected AppContainer identity");
    const auto capability_data = token_information(token.value, TokenCapabilities);
    const auto* actual_capabilities = reinterpret_cast<const TOKEN_GROUPS*>(capability_data.data());
    if (actual_capabilities->GroupCount != (impl_->registry_read ? 1U : 0U) ||
        (impl_->registry_read && (!EqualSid(actual_capabilities->Groups[0].Sid,impl_->registry_sid.data()) ||
            actual_capabilities->Groups[0].Attributes != SE_GROUP_ENABLED)))
        throw std::runtime_error("Sandbox process unexpectedly has capabilities");
    const auto integrity = token_information(token.value, TokenIntegrityLevel);
    const auto integrity_sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
    if (!IsValidSid(integrity_sid) || !*GetSidSubAuthorityCount(integrity_sid) ||
        *GetSidSubAuthority(integrity_sid, *GetSidSubAuthorityCount(integrity_sid) - 1) > SECURITY_MANDATORY_LOW_RID)
        throw std::runtime_error("Sandbox process integrity exceeds low integrity");
}
}
#endif
