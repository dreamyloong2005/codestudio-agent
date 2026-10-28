#include "codestudio/build_workspace.hpp"
#include "codestudio/store.hpp"
#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace codestudio {
namespace {
namespace fs = std::filesystem;
[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("build workspace: " + message);
}
#ifdef _WIN32
[[noreturn]] void winfail(const char* operation, const fs::path& path) {
    const auto error = GetLastError();
    fail(std::string(operation) + " " + path_utf8(path) + " (Windows error " + std::to_string(error) + ")");
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}
bool same(const fs::path& left, const fs::path& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::string manifest_path(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
fs::path joined(const fs::path& root, const fs::path& relative_path) { return relative_path.empty() ? root : root / relative_path; }
bool under(const fs::path& candidate, const fs::path& prefix) {
    auto a = candidate.begin(), b = prefix.begin();
    for (; b != prefix.end(); ++a, ++b) if (a == candidate.end() || !same(*a, *b)) return false;
    return true;
}
void component(const fs::path& name) {
    const auto value = name.native();
    if (value.empty() || value == L"." || value == L".." || value.back() == L'.' || value.back() == L' ')
        fail("invalid path component: " + path_utf8(name));
    for (const auto c : value) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos)
        fail("invalid path component: " + path_utf8(name));
    const auto stem = lower(value.substr(0, value.find(L'.')));
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" || stem == L"conin$" || stem == L"conout$" ||
        (stem.size() == 4 && (stem.starts_with(L"com") || stem.starts_with(L"lpt")) &&
         ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3')))
        fail("reserved device name: " + path_utf8(name));
}
fs::path checked_relative(const fs::path& path, bool dot = true) {
    if (path.empty() || path.has_root_path()) fail("expected explicit relative path: " + path_utf8(path));
    if (path == L"." && dot) return {};
    fs::path result;
    for (const auto& part : path) { component(part); result /= part; }
    return result;
}
fs::path checked_absolute(const fs::path& path) {
    if (!path.is_absolute() || path.root_name().native().size() != 2 || path.root_name().native()[1] != L':')
        fail("expected absolute local-drive path: " + path_utf8(path));
    for (const auto& part : path.relative_path()) component(part);
    const auto root = path.root_path();
    const auto type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_RAMDISK) fail("workspace must be on a fixed local drive");
    return path.lexically_normal();
}
BY_HANDLE_FILE_INFORMATION info(HANDLE handle, const fs::path& path) {
    BY_HANDLE_FILE_INFORMATION result{};
    if (!GetFileInformationByHandle(handle, &result)) winfail("inspect", path);
    return result;
}
void verify_opened_path(HANDLE handle, const fs::path& expected) {
    // Reparse checks alone do not reject DOS 8.3 aliases. Those could make a
    // lexical protected-path comparison refer to a different on-disk name.
    const auto length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length) winfail("resolve opened path", expected);
    std::wstring value(length, L'\0');
    const auto written = GetFinalPathNameByHandleW(handle, value.data(), length, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!written || written >= length) winfail("resolve opened path", expected);
    value.resize(written);
    if (!value.starts_with(L"\\\\?\\")) fail("opened path is not a local DOS path");
    value.erase(0, 4);
    if (!same(fs::path(value).lexically_normal(), expected.lexically_normal()))
        fail("filesystem path aliases are forbidden: " + path_utf8(expected));
}
Handle directory(const fs::path& path) {
    Handle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) winfail("open directory", path);
    const auto data = info(handle.value, path);
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        fail("directory is a link or not a directory: " + path_utf8(path));
    verify_opened_path(handle.value, path);
    return handle;
}
std::vector<Handle> anchor(const fs::path& root) {
    std::vector<Handle> handles;
    auto path = root.root_path();
    handles.push_back(directory(path));
    for (const auto& part : root.relative_path()) { path /= part; handles.push_back(directory(path)); }
    return handles;
}
Handle make_directory(const fs::path& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) winfail("create directory", path);
    return directory(path);
}
bool reserved(const fs::path& path) {
    for (const auto& part : path) if (same(part, L".git") || same(part, L".codestudio")) return true;
    return false;
}
std::vector<fs::path> protections(const fs::path& root, const std::vector<fs::path>& values) {
    std::vector<fs::path> result;
    for (const auto& value : values) {
        auto path = value.is_absolute() ? checked_absolute(value) : joined(root, checked_relative(value));
        if (under(path, root)) {
            auto part = path.begin();
            for (auto base = root.begin(); base != root.end(); ++base) ++part;
            fs::path suffix;
            for (; part != path.end(); ++part) suffix /= *part;
            result.push_back(std::move(suffix));
        }
    }
    return result;
}
bool matches(const fs::path& path, const std::vector<fs::path>& prefixes) {
    return std::any_of(prefixes.begin(), prefixes.end(), [&](const auto& prefix) { return prefix.empty() || prefix == L"." || under(path, prefix); });
}
std::vector<fs::path> selections(const std::vector<fs::path>& values) {
    if (values.empty()) fail("at least one explicit input/output path is required");
    std::vector<fs::path> result;
    for (const auto& value : values) {
        auto path = checked_relative(value);
        if (std::any_of(result.begin(), result.end(), [&](const auto& prior) { return prior.empty() || under(path, prior); })) continue;
        std::erase_if(result, [&](const auto& prior) { return path.empty() || under(prior, path); });
        result.push_back(std::move(path));
    }
    return result;
}
struct Digest {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Digest() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) fail("open SHA256 provider");
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr; fail("create SHA256 digest");
        }
    }
    ~Digest() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    void add(unsigned char* data, ULONG count) { if (BCryptHashData(hash, data, count, 0) < 0) fail("update SHA256 digest"); }
    std::string finish() {
        std::array<unsigned char, 32> bytes{};
        if (BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) fail("finish SHA256 digest");
        constexpr char hex[] = "0123456789abcdef";
        std::string result; result.reserve(64);
        for (const auto value : bytes) { result += hex[value >> 4]; result += hex[value & 15]; }
        return result;
    }
};
struct Copier {
    fs::path source, destination;
    BuildCopyLimits limits;
    const std::atomic_bool& cancelled;
    std::vector<fs::path> excluded, protected_paths;
    bool workspace_exclusions = true, exporting = false;
    BuildCopyStats stats;
    std::size_t entries = 0;
    Json files = Json::array();
    void tick() const { if (cancelled.load()) fail("cancelled"); }
    bool excluded_path(const fs::path& path) const {
        return (workspace_exclusions && reserved(path)) || matches(path, excluded) || matches(path, protected_paths);
    }
    void file(const fs::path& rel, const fs::path& from, const fs::path& to) {
        Handle input(CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (input.value == INVALID_HANDLE_VALUE) winfail("open input", from);
        const auto metadata = info(input.value, from);
        if (metadata.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) fail("input is not a regular file: " + path_utf8(from));
        if (GetFileType(input.value) != FILE_TYPE_DISK) fail("input is not a disk file");
        verify_opened_path(input.value, from);
        if (metadata.nNumberOfLinks != 1) fail("hard-linked input or artifact rejected: " + path_utf8(from));
        const auto size = (static_cast<std::uint64_t>(metadata.nFileSizeHigh) << 32) | metadata.nFileSizeLow;
        if (stats.files >= limits.max_files || size > limits.max_file_bytes || stats.bytes > limits.max_bytes || size > limits.max_bytes - stats.bytes)
            fail("file/byte import or export budget exceeded");
        Handle output(CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (output.value == INVALID_HANDLE_VALUE) winfail("create output", to);
        std::optional<Digest> digest;
        if (exporting) digest.emplace();
        std::array<unsigned char, 65536> buffer{};
        std::uint64_t total = 0;
        while (true) {
            tick(); DWORD count = 0;
            if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) winfail("read input", from);
            if (!count) break;
            if (count > size - total) fail("input changed while copying");
            DWORD offset = 0;
            while (offset < count) {
                tick(); DWORD written = 0;
                if (!WriteFile(output.value, buffer.data() + offset, count - offset, &written, nullptr) || !written) winfail("write output", to);
                offset += written;
            }
            if (exporting) digest->add(buffer.data(), count);
            total += count;
        }
        if (total != size) fail("input changed while copying");
        if (exporting) {
            if (!FlushFileBuffers(output.value)) winfail("flush artifact", to);
            files.push_back({{"path", manifest_path(rel)}, {"artifact_path", "files/" + manifest_path(rel)},
                {"bytes", size}, {"sha256", digest->finish()}});
        }
        ++stats.files; stats.bytes += size;
    }
    void node(const fs::path& rel, std::size_t depth, bool explicit_selection) {
        tick();
        if (depth > limits.max_depth || ++entries > limits.max_entries) fail("directory depth/entry budget exceeded");
        if (excluded_path(rel)) {
            if (explicit_selection || exporting) fail("protected or excluded path selected: " + path_utf8(rel));
            return;
        }
        const auto from = joined(source, rel), to = joined(destination, rel);
        const auto attributes = GetFileAttributesW(from.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) winfail("inspect input", from);
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) fail("symbolic link or reparse point rejected: " + path_utf8(from));
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { file(rel, from, to); return; }
        auto input = directory(from);
        auto output = directory(to); // Destination directory created before recursion.
        WIN32_FIND_DATAW entry{};
        const auto search = FindFirstFileW((from / L"*").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return;
            winfail("enumerate directory", from);
        }
        struct FindGuard { HANDLE value; ~FindGuard() { FindClose(value); } } guard{search};
        while (true) {
            const fs::path name(entry.cFileName);
            if (name != L"." && name != L"..") {
                component(name); tick();
                const auto child = rel / name;
                if (!excluded_path(child) && (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    auto created = make_directory(destination / child);
                    node(child, depth + 1, false);
                } else node(child, depth + 1, false);
            }
            if (!FindNextFileW(search, &entry)) {
                if (GetLastError() != ERROR_NO_MORE_FILES) winfail("enumerate directory", from);
                break;
            }
        }
    }
    void run(const std::vector<fs::path>& selected) {
        for (const auto& rel : selected) {
            tick();
            if (excluded_path(rel)) fail("protected or excluded path selected: " + path_utf8(rel));
            auto source_path = source, target_path = destination;
            std::vector<Handle> parents;
            std::size_t depth = 0;
            for (const auto& part : rel.parent_path()) {
                if (++depth > limits.max_depth) fail("directory depth budget exceeded");
                source_path /= part; target_path /= part;
                parents.push_back(directory(source_path));
                parents.push_back(make_directory(target_path));
            }
            const auto attributes = GetFileAttributesW(joined(source, rel).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) winfail("inspect selection", joined(source, rel));
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                auto created = make_directory(joined(destination, rel));
                node(rel, depth + (rel.empty() ? 0 : 1), true);
            } else node(rel, depth + (rel.empty() ? 0 : 1), true);
        }
        tick();
    }
};
struct Measurer {
    const BuildCopyLimits& limits;
    const std::atomic_bool& cancelled;
    BuildCopyStats stats;
    std::size_t entries = 0;
    void node(const fs::path& path, std::size_t depth) {
        if (cancelled.load()) fail("cancelled");
        if (depth > limits.max_depth || ++entries > limits.max_entries) fail("directory depth/entry measurement budget exceeded");
        Handle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (handle.value == INVALID_HANDLE_VALUE) winfail("open measurement entry", path);
        const auto data = info(handle.value, path);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) fail("reparse point rejected during measurement: " + path_utf8(path));
        verify_opened_path(handle.value, path);
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (GetFileType(handle.value) != FILE_TYPE_DISK || data.nNumberOfLinks != 1) fail("non-regular or hard-linked file rejected during measurement");
            const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (stats.files >= limits.max_files || size > limits.max_file_bytes || stats.bytes > limits.max_bytes || size > limits.max_bytes - stats.bytes)
                fail("file/byte measurement budget exceeded");
            ++stats.files; stats.bytes += size;
            return;
        }
        WIN32_FIND_DATAW entry{};
        const auto search = FindFirstFileW((path / L"*").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return;
            winfail("enumerate measurement directory", path);
        }
        struct FindGuard { HANDLE value; ~FindGuard() { FindClose(value); } } guard{search};
        while (true) {
            const fs::path name(entry.cFileName);
            if (name != L"." && name != L"..") { component(name); node(path / name, depth + 1); }
            if (!FindNextFileW(search, &entry)) {
                if (GetLastError() != ERROR_NO_MORE_FILES) winfail("enumerate measurement directory", path);
                break;
            }
        }
    }
};
void remove_node(const fs::path& path, std::size_t depth, std::size_t& remaining) {
    if (depth > 128 || !remaining) fail("cleanup budget exceeded; retained owned tree");
    --remaining;
    Handle handle(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return;
        winfail("open cleanup entry", path);
    }
    const auto data = info(handle.value, path);
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) verify_opened_path(handle.value, path);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW item{};
        auto search = FindFirstFileW((path / L"*").c_str(), &item);
        if (search == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) winfail("enumerate cleanup", path);
        } else {
            struct FindGuard { HANDLE value; ~FindGuard() { FindClose(value); } } guard{search};
            while (true) {
                const fs::path name(item.cFileName);
                if (name != L"." && name != L"..") { component(name); remove_node(path / name, depth + 1, remaining); }
                if (!FindNextFileW(search, &item)) {
                    if (GetLastError() != ERROR_NO_MORE_FILES) winfail("enumerate cleanup", path);
                    break;
                }
            }
        }
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
        // Attributes belong to the underlying file, not this directory entry.
        // Do not mutate an external hardlink's attributes merely to clean up.
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && data.nNumberOfLinks != 1)
            fail("retaining read-only hardlink during cleanup: " + path_utf8(path));
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(handle.value, FileBasicInfo, &basic, sizeof(basic))) winfail("inspect cleanup attributes", path);
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(handle.value, FileBasicInfo, &basic, sizeof(basic))) winfail("clear owned read-only attribute", path);
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof(disposition))) winfail("delete owned entry", path);
}
void write_manifest(const fs::path& path, const Json& value, std::size_t max_bytes) {
    const auto text = value.dump(2) + "\n";
    if (text.size() > max_bytes) fail("serialized artifact manifest exceeds byte limit");
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) winfail("create artifact manifest", path);
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 65536));
        DWORD written = 0;
        if (!WriteFile(file.value, text.data() + offset, chunk, &written, nullptr) || !written) winfail("write artifact manifest", path);
        offset += written;
    }
    if (!FlushFileBuffers(file.value)) winfail("flush artifact manifest", path);
}
#endif
}

#ifndef __linux__
#ifndef __APPLE__
std::string read_build_manifest_snapshot(const fs::path& manifest, std::size_t max_bytes) {
#ifdef _WIN32
    const auto path = checked_absolute(manifest);
    auto parents = anchor(path.parent_path());
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) winfail("open build manifest", path);
    const auto data = info(file.value, path);
    if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        GetFileType(file.value) != FILE_TYPE_DISK || data.nNumberOfLinks != 1)
        fail("build manifest must be a regular file without links");
    verify_opened_path(file.value, path);
    const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size > max_bytes) fail("build manifest exceeds byte limit");
    std::string result; result.reserve(static_cast<std::size_t>(size));
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD count = 0;
        if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) winfail("read build manifest", path);
        if (!count) break;
        if (count > size - result.size()) fail("build manifest changed while reading");
        result.append(buffer.data(), count);
    }
    if (result.size() != size) fail("build manifest changed while reading");
    return result;
#else
    (void)manifest; (void)max_bytes;
    fail("safe build manifests currently require Windows");
#endif
}
#endif

BuildCopyStats import_build_tree(const fs::path& source_root, const fs::path& destination,
    const BuildImportOptions& options, const std::atomic_bool& cancelled) {
#ifdef _WIN32
    const auto source = checked_absolute(source_root), target = checked_absolute(destination);
    if (under(source, target) || under(target, source)) fail("import source and destination must be disjoint");
    auto source_anchor = anchor(source), target_anchor = anchor(target);
    Copier copier{source, target, options.limits, cancelled};
    copier.workspace_exclusions = options.workspace_exclusions;
    copier.protected_paths = protections(source, options.protected_paths);
    for (const auto& excluded : options.excludes) copier.excluded.push_back(checked_relative(excluded));
    // Validate every explicit request before deduplicating overlapping roots.
    for (const auto& value : options.inputs) {
        const auto rel = checked_relative(value);
        if (copier.excluded_path(rel)) fail("protected or excluded path selected: " + path_utf8(rel));
        if (GetFileAttributesW(joined(source, rel).c_str()) == INVALID_FILE_ATTRIBUTES) winfail("inspect selection", joined(source, rel));
    }
    copier.run(selections(options.inputs));
    return copier.stats;
#else
    (void)source_root; (void)destination; (void)options; (void)cancelled;
    fail("safe workspace copying is currently supported only on Windows");
#endif
}

Json export_build_artifacts(const fs::path& private_workspace, const std::vector<fs::path>& outputs,
    const fs::path& host_workspace, const fs::path& artifact_directory,
    const std::vector<fs::path>& protected_paths, const BuildCopyLimits& limits,
    const std::atomic_bool& cancelled) {
#ifdef _WIN32
    const auto source = checked_absolute(private_workspace), host = checked_absolute(host_workspace);
    if (under(source, host) || under(host, source)) fail("artifact source and host workspace must be disjoint");
    const auto artifact_rel = checked_relative(artifact_directory, false);
    const auto protected_rel = protections(host, protected_paths);
    if (reserved(artifact_rel) || matches(artifact_rel, protected_rel)) fail("protected artifact destination");
    const auto selected = selections(outputs);
    auto source_anchor = anchor(source), host_anchor = anchor(host);
    for (const auto& value : outputs) {
        const auto rel = checked_relative(value);
        if (reserved(rel)) fail("protected output selected: " + path_utf8(rel));
        if (GetFileAttributesW(joined(source, rel).c_str()) == INVALID_FILE_ATTRIBUTES) winfail("inspect output selection", joined(source, rel));
    }
    auto parent = host;
    for (const auto& part : artifact_rel) { parent /= part; host_anchor.push_back(make_directory(parent)); }
    const auto id = new_id();
    const auto staging = parent / utf8_path(".codestudio-staging-" + id);
    const auto published = parent / utf8_path(id);
    if (!CreateDirectoryW(staging.c_str(), nullptr)) winfail("create artifact staging directory", staging);
    try {
        Json manifest;
        {
            auto staging_anchor = directory(staging);
            auto files_anchor = make_directory(staging / L"files");
            Copier copier{source, staging / L"files", limits, cancelled};
            copier.exporting = true;
            copier.run(selected);
            manifest = {{"schema_version", 2}, {"files", copier.files}, {"file_count", copier.stats.files}, {"bytes", copier.stats.bytes},
                {"directory", path_utf8(published)}, {"manifest", path_utf8(published / L"manifest.json")}};
            write_manifest(staging / L"manifest.json", manifest, limits.max_manifest_bytes);
            copier.tick();
        }
        if (cancelled.load()) fail("cancelled before artifact publication");
        // Same-volume rename, without REPLACE_EXISTING or COPY_ALLOWED. Holding
        // all host ancestors without delete sharing prevents directory swaps.
        if (!MoveFileExW(staging.c_str(), published.c_str(), MOVEFILE_WRITE_THROUGH)) winfail("publish artifact bundle", published);
        return manifest;
    } catch (...) {
        try { safe_remove_owned_tree(staging); } catch (...) { /* Retain residue rather than follow links or widen ACLs. */ }
        throw;
    }
#else
    (void)private_workspace; (void)outputs; (void)host_workspace; (void)artifact_directory;
    (void)protected_paths; (void)limits; (void)cancelled;
    fail("safe artifact export is currently supported only on Windows");
#endif
}

BuildCopyStats measure_build_tree(const fs::path& root, const BuildCopyLimits& limits, const std::atomic_bool& cancelled) {
#ifdef _WIN32
    const auto path = checked_absolute(root);
    auto parents = anchor(path);
    Measurer measurer{limits, cancelled};
    measurer.node(path, 0);
    if (cancelled.load()) fail("cancelled");
    return measurer.stats;
#else
    (void)root; (void)limits; (void)cancelled;
    fail("safe workspace measurement is currently supported only on Windows");
#endif
}

void safe_remove_owned_tree(const fs::path& owned_tree) {
#ifdef _WIN32
    const auto path = checked_absolute(owned_tree);
    if (path == path.root_path()) fail("refusing cleanup of drive root");
    auto parents = anchor(path.parent_path());
    std::size_t remaining = 1000000;
    remove_node(path, 0, remaining);
#else
    (void)owned_tree;
    fail("safe owned-tree cleanup is currently supported only on Windows");
#endif
}
#endif
}
