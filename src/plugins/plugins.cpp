#include "codestudio/plugins.hpp"
#include "codestudio/build_workspace.hpp"
#include "codestudio/digest.hpp"
#include "codestudio/process_package.hpp"
#include "../provider/protocol.hpp"
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/fs.h>
#endif
#ifdef __APPLE__
#include <stdio.h>
#endif
#include <unistd.h>
#endif

namespace codestudio {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t max_payload = 64 * 1024 * 1024;
constexpr std::size_t max_files = 128;
constexpr std::size_t max_metadata = 1024 * 1024;
[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("Plugin: ") + message); }
void require(bool value, const char* message) { if (!value) fail(message); }
std::string utf8(const fs::path& path) { const auto value = path.generic_u8string(); return {value.begin(), value.end()}; }
fs::path path_of(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
fs::path checked_absolute(fs::path path) {
    require(!path.empty(), "empty filesystem path");
    for (const auto& item : path) require(item != "..", "parent path traversal is forbidden");
    path = fs::absolute(path).lexically_normal();
    if (!path.has_filename() && path != path.root_path()) path = path.parent_path();
    require(path.has_filename(), "a filesystem root is not an output path");
    return path;
}
bool plain(std::string_view text, std::size_t maximum, bool empty = false) {
    return (empty || !text.empty()) && text.size() <= maximum &&
        std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string string(const Json& object, const char* key, std::size_t maximum, bool empty = false) {
    require(object.contains(key) && object[key].is_string(), "missing string field");
    auto value = object[key].get<std::string>();
    require(plain(value, maximum, empty), "invalid string field");
    return value;
}
void keys(const Json& object, std::initializer_list<std::string_view> expected) {
    require(object.is_object() && object.size() == expected.size(), "invalid metadata fields");
    for (const auto field : expected) require(object.contains(std::string(field)), "invalid metadata fields");
}
Json parse(const std::string& data) {
    try { return protocol::strict_json(data); }
    catch (...) { fail("invalid metadata JSON"); }
}
std::string encoded(const Json& value) { return value.dump() + "\n"; }
bool hex_digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool name(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
void version(const std::string& value) {
    std::size_t start = 0;
    for (int index = 0; index < 3; ++index) {
        const auto end = value.find('.', start);
        const auto part = std::string_view(value).substr(start, end == std::string::npos ? end : end - start);
        require(!part.empty() && part.size() <= 6 && (part.size() == 1 || part.front() != '0') &&
            std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }), "version must be exact major.minor.patch");
        require((index == 2) == (end == std::string::npos), "version must be exact major.minor.patch");
        start = end + 1;
    }
}
std::string relative_name(const std::string& value) {
    require(plain(value, 240) && value.front() != '/' && value.back() != '/' &&
            value.find_first_of("\\:") == std::string::npos, "invalid package path");
    for (const auto& part : path_of(value)) {
        const auto item = utf8(part);
        require(!item.empty() && item != "." && item != ".." && item.back() != '.' &&
            std::all_of(item.begin(), item.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            }), "invalid package path");
        const auto base = lower(item.substr(0, item.find('.')));
        require(base != "con" && base != "prn" && base != "aux" && base != "nul" &&
            !(base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9'),
            "package path uses a reserved device name");
    }
    require(utf8(path_of(value).lexically_normal()) == value, "noncanonical package path");
    return value;
}
std::pair<std::string, std::string> native() {
#ifdef _WIN32
    return {"windows", "x64"};
#elif defined(__linux__) && defined(__x86_64__)
    return {"linux", "x86_64"};
#elif defined(__APPLE__) && defined(__aarch64__)
    return {"macosx", "arm64"};
#else
    fail("native plugins are unavailable for this platform/architecture");
#endif
}
std::string unique() {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (int count = 0; count < 4; ++count) {
        const auto value = static_cast<std::uint32_t>(random());
        for (unsigned index = 0; index < 8; ++index) result += digits[(value >> (index * 4)) & 15];
    }
    return result;
}

// Hold every ancestor while writing. Linux operations use the opened parent
// descriptor and verify every directory identity; Windows forbids renames via
// handles without FILE_SHARE_DELETE. No symlink/reparse resolution is accepted.
class Directory {
public:
    explicit Directory(const fs::path& path, bool create = false) : path_(path) {
#ifdef _WIN32
        try {
            auto current = path.root_path();
            open(current);
            for (const auto& part : path.relative_path()) {
                current /= part;
                if (create && !CreateDirectoryW(current.c_str(), nullptr))
                    require(GetLastError() == ERROR_ALREADY_EXISTS, "cannot create installation directory");
                open(current);
            }
        } catch (...) { close(); throw; }
#else
        const auto root = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        require(root >= 0, "cannot anchor filesystem root");
        handles_.push_back(root);
        try {
            for (const auto& part : path.relative_path()) {
                const auto item = part.native();
                if (create && mkdirat(handles_.back(), item.c_str(), 0700) != 0)
                    require(errno == EEXIST, "cannot create installation directory");
                const auto next = openat(handles_.back(), item.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                require(next >= 0, "directory contains a link or is unavailable");
                handles_.push_back(next);
                names_.push_back(item);
            }
            verify();
        } catch (...) { close(); throw; }
#endif
    }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    ~Directory() { close(); }
    void verify() const {
#ifndef _WIN32
        for (std::size_t index = 1; index < handles_.size(); ++index) {
            struct stat named{}, opened{};
            require(fstatat(handles_[index - 1], names_[index - 1].c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                fstat(handles_[index], &opened) == 0 && S_ISDIR(named.st_mode) &&
                named.st_dev == opened.st_dev && named.st_ino == opened.st_ino, "installation directory changed during operation");
        }
#endif
    }
    void create_directory(const std::string& item) const {
        verify();
#ifdef _WIN32
        require(CreateDirectoryW((path_ / path_of(item)).c_str(), nullptr) != 0, "cannot create unique staging directory");
#else
        require(mkdirat(handles_.back(), item.c_str(), 0700) == 0, "cannot create unique staging directory");
#endif
        verify();
    }
    void write(const std::string& item, const std::string& bytes, bool executable) const {
        verify();
#ifdef _WIN32
        const auto target = path_ / path_of(item);
        const auto handle = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "output already exists or cannot be created");
        bool okay = true;
        for (std::size_t offset = 0; okay && offset < bytes.size();) {
            const auto length = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536));
            DWORD count = 0;
            okay = WriteFile(handle, bytes.data() + offset, length, &count, nullptr) && count == length;
            offset += count;
        }
        okay = FlushFileBuffers(handle) && okay;
        CloseHandle(handle);
        require(okay, "output write failed; partial file retained for inspection");
        (void)executable;
#else
        const auto descriptor = openat(handles_.back(), item.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        require(descriptor >= 0, "output already exists or cannot be created");
        bool okay = true;
        for (std::size_t offset = 0; offset < bytes.size();) {
            const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { okay = false; break; }
            offset += static_cast<std::size_t>(count);
        }
        okay = fchmod(descriptor, executable ? 0700 : 0600) == 0 && fsync(descriptor) == 0 && okay;
        ::close(descriptor);
        require(okay, "output write failed; partial file retained for inspection");
#endif
        verify();
    }
    void publish(const std::string& from, const std::string& to, bool replace) const {
        verify();
#ifdef _WIN32
        require(MoveFileExW((path_ / path_of(from)).c_str(), (path_ / path_of(to)).c_str(),
                           MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != 0,
                "publication failed; staging path retained for inspection");
#else
#ifdef __APPLE__
        const auto code = renameatx_np(handles_.back(), from.c_str(), handles_.back(), to.c_str(),
                                      replace ? 0 : RENAME_EXCL);
#else
        const auto code = replace ? renameat(handles_.back(), from.c_str(), handles_.back(), to.c_str()) :
            syscall(SYS_renameat2, handles_.back(), from.c_str(), handles_.back(), to.c_str(), RENAME_NOREPLACE);
#endif
        require(code == 0,
                "publication failed; staging path retained for inspection");
        require(fsync(handles_.back()) == 0, "publication directory sync failed");
#endif
        verify();
    }
    void remove_file(const std::string& item) const {
        verify();
#ifdef _WIN32
        require(DeleteFileW((path_ / path_of(item)).c_str()) != 0, "cannot release plugin operation lock");
#else
        require(unlinkat(handles_.back(), item.c_str(), 0) == 0, "cannot release plugin operation lock");
#endif
        verify();
    }
private:
    fs::path path_;
#ifdef _WIN32
    std::vector<HANDLE> handles_;
    void open(const fs::path& path) {
        const auto handle = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "cannot anchor installation directory");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { CloseHandle(handle); fail("directory contains a link or reparse point"); }
        handles_.push_back(handle);
    }
    void close() { for (const auto handle : handles_) CloseHandle(handle); handles_.clear(); }
#else
    std::vector<int> handles_;
    std::vector<std::string> names_;
    void close() { for (const auto descriptor : handles_) ::close(descriptor); handles_.clear(); }
#endif
};

class MutationLock {
public:
    MutationLock(const fs::path& parent, std::string name) : directory_(parent), path_(parent / path_of(name)), name_(std::move(name)), token_(unique()) {
        directory_.write(name_, token_, false);
    }
    ~MutationLock() {
        try { if (read_build_manifest_snapshot(path_, 64) == token_) directory_.remove_file(name_); } catch (...) {}
    }
private:
    Directory directory_;
    fs::path path_;
    std::string name_, token_;
};

bool executable(const fs::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "linked package file");
    return lower(utf8(path.extension())) == ".exe";
#else
    struct stat info{};
    require(lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
            !(info.st_mode & (S_ISUID | S_ISGID)), "linked or privileged package file");
    return (info.st_mode & 0111) != 0;
#endif
}
std::set<std::string> inventory(const fs::path& root) {
    Directory anchor(root);
    std::set<std::string> paths, aliases;
    std::size_t count = 0;
    for (const auto& item : fs::recursive_directory_iterator(root)) {
        require(++count <= 512, "package has too many directory entries");
        const auto path = item.path();
        const auto relative = relative_name(utf8(path.lexically_relative(root)));
        require(aliases.insert(lower(relative)).second, "case-insensitive package path collision");
        const auto status = item.symlink_status();
        require(!fs::is_symlink(status), "package links are forbidden");
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "package reparse points are forbidden");
#endif
        if (fs::is_directory(status)) continue;
        require(fs::is_regular_file(status), "package has a nonregular entry");
        // This anchored reader checks single-link identity on both platforms.
        paths.insert(relative);
    }
    anchor.verify();
    return paths;
}
std::set<std::string> directories(const fs::path& root) {
    std::set<std::string> result;
    for (const auto& item : fs::recursive_directory_iterator(root))
        if (item.is_directory()) result.insert(utf8(item.path().lexically_relative(root)));
    return result;
}
std::set<std::string> parents(const std::set<std::string>& files) {
    std::set<std::string> result;
    for (const auto& name : files)
        for (auto parent = path_of(name).parent_path(); !parent.empty(); parent = parent.parent_path()) result.insert(utf8(parent));
    return result;
}
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(std::string_view value) {
    std::string result;
    result.reserve((value.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < value.size(); index += 3) {
        const auto a = static_cast<unsigned char>(value[index]);
        const auto b = index + 1 < value.size() ? static_cast<unsigned char>(value[index + 1]) : 0;
        const auto c = index + 2 < value.size() ? static_cast<unsigned char>(value[index + 2]) : 0;
        result += alphabet[a >> 2]; result += alphabet[((a & 3) << 4) | (b >> 4)];
        result += index + 1 < value.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        result += index + 2 < value.size() ? alphabet[c & 63] : '=';
    }
    return result;
}
std::string unbase64(const std::string& value, std::size_t size) {
    require(value.size() == (size + 2) / 3 * 4, "payload encoding size differs from manifest");
    std::array<int, 256> values; values.fill(-1);
    for (int index = 0; index < 64; ++index) values[static_cast<unsigned char>(alphabet[index])] = index;
    std::string result; result.reserve(size);
    for (std::size_t index = 0; index < value.size(); index += 4) {
        const auto a = values[static_cast<unsigned char>(value[index])], b = values[static_cast<unsigned char>(value[index + 1])];
        const auto c = value[index + 2] == '=' ? 0 : values[static_cast<unsigned char>(value[index + 2])];
        const auto d = value[index + 3] == '=' ? 0 : values[static_cast<unsigned char>(value[index + 3])];
        require(a >= 0 && b >= 0 && c >= 0 && d >= 0, "invalid payload encoding");
        result += static_cast<char>((a << 2) | (b >> 4));
        if (result.size() < size) result += static_cast<char>((b << 4) | (c >> 2));
        if (result.size() < size) result += static_cast<char>((c << 6) | d);
    }
    require(result.size() == size && base64(result) == value, "noncanonical payload encoding");
    return result;
}

void descriptor(const Json& manifest, bool published) {
    if (published) keys(manifest, {"format", "id", "version", "platform", "arch", "entrypoint", "args", "tools", "files"});
    else keys(manifest, {"format", "id", "version", "platform", "arch", "entrypoint", "args", "tools"});
    require(manifest["format"].is_number_integer() && manifest["format"] == 2, "unsupported plugin format");
    const auto id = string(manifest, "id", 64);
    require(name(id, 64) && id == lower(id), "plugin id must be lowercase ASCII letters, digits, underscore or hyphen");
    relative_name(id);
    version(string(manifest, "version", 20));
    const auto [platform, arch] = native();
    require(manifest["platform"] == platform && manifest["arch"] == arch, "plugin target differs from the native platform");
    relative_name(string(manifest, "entrypoint", 240));
    require(manifest["args"].is_array() && manifest["args"].size() <= 128, "invalid plugin arguments");
    std::size_t bytes = 0;
    for (const auto& value : manifest["args"]) {
        require(value.is_string(), "invalid plugin argument");
        const auto text = value.get<std::string>();
        require(plain(text, 32768, true) && (bytes += text.size()) <= 65536, "invalid plugin argument");
        if (text.starts_with("${plugin}/")) relative_name(text.substr(10));
        else require(text.find("${") == std::string::npos, "unsupported plugin argument template");
    }
    require(manifest["tools"].is_array() && !manifest["tools"].empty() && manifest["tools"].size() <= 128,
            "plugin needs an exact nonempty tool declaration");
    std::set<std::string> tools;
    for (const auto& value : manifest["tools"]) {
        require(value.is_string() && name(value.get<std::string>(), 64) && tools.insert(value.get<std::string>()).second,
                "invalid or repeated plugin tool name");
    }
}
struct Package {
    Json manifest;
    std::string bytes, hash;
    std::map<std::string, std::string> files;
    std::map<std::string, bool> modes;
};
Package package(const std::string& bytes, const std::string& expected) {
    require(bytes.size() <= plugin_detail::max_bundle && hex_digest(expected), "invalid bundle size or expected digest");
    require(sha256_bytes(bytes) == expected, "bundle SHA256 mismatch");
    const auto object = parse(bytes);
    keys(object, {"format", "manifest", "payloads"});
    require(object["format"].is_number_integer() && object["format"] == 2, "unsupported bundle format");
    Package result{object["manifest"], bytes, expected, {}, {}};
    descriptor(result.manifest, true);
    const auto& records = result.manifest["files"];
    require(records.is_array() && !records.empty() && records.size() <= max_files &&
            object["payloads"].is_object() && object["payloads"].size() == records.size(), "invalid bundle inventory");
    std::set<std::string> aliases, names;
    std::size_t total = 0;
    for (const auto& record : records) {
        keys(record, {"path", "sha256", "size", "executable"});
        const auto file = relative_name(string(record, "path", 240));
        require(lower(file) != "plugin.json" && lower(file) != "bundle.csplugin" && aliases.insert(lower(file)).second,
                "duplicate or reserved payload name");
        require(record["size"].is_number_integer() && record["size"] >= 0 && record["size"] <= max_payload &&
                record["executable"].is_boolean(), "invalid file metadata");
        const auto size = record["size"].get<std::size_t>();
        require(size <= max_payload - total, "plugin payload exceeds total byte limit");
        total += size;
        const auto hash = string(record, "sha256", 64);
        require(hex_digest(hash) && object["payloads"].contains(file) && object["payloads"][file].is_string(), "invalid payload digest or body");
        auto data = unbase64(object["payloads"][file].get<std::string>(), size);
        require(sha256_bytes(data) == hash, "payload SHA256 mismatch");
        result.files.emplace(file, std::move(data)); result.modes.emplace(file, record["executable"].get<bool>());
        names.insert(file);
    }
    const auto dirs = parents(names);
    for (const auto& dir : dirs) require(!aliases.contains(lower(dir)), "file collides with package directory");
    std::set<std::string> directory_aliases;
    for (const auto& dir : dirs) require(directory_aliases.insert(lower(dir)).second, "directory name alias collision");
    const auto entry = result.manifest["entrypoint"].get<std::string>();
    require(result.files.contains(entry) && result.modes.at(entry), "entrypoint must be an executable payload file");
    for (const auto& value : result.manifest["args"]) {
        const auto argument = value.get<std::string>();
        if (argument.starts_with("${plugin}/")) require(names.contains(argument.substr(10)) || dirs.contains(argument.substr(10)),
                "plugin argument names an absent payload path");
    }
    require(bytes == encoded(object), "bundle must use canonical format-2 serialization");
    return result;
}
Json identity(const Package& value) {
    return {{"format", 2}, {"id", value.manifest["id"]}, {"version", value.manifest["version"]},
            {"platform", value.manifest["platform"]}, {"arch", value.manifest["arch"]}, {"bundle_sha256", value.hash}};
}
void verify_installation(const fs::path& path, const Package& value) {
    const auto files = inventory(path);
    std::set<std::string> expected{"plugin.json", "bundle.csplugin"};
    for (const auto& [name, data] : value.files) { (void)data; expected.insert(name); }
    require(files == expected && directories(path) == parents(expected), "installed inventory has changed");
    require(read_build_manifest_snapshot(path / "plugin.json", max_metadata) == encoded(value.manifest) &&
            read_build_manifest_snapshot(path / "bundle.csplugin", plugin_detail::max_bundle) == value.bytes,
            "installed manifest or original bundle has changed");
#ifndef _WIN32
    require(!executable(path / "plugin.json") && !executable(path / "bundle.csplugin"), "installed metadata modes have changed");
#endif
    for (const auto& [name, bytes] : value.files) {
        const auto file = path / path_of(name);
        require(read_build_manifest_snapshot(file, max_payload) == bytes, "installed payload has changed");
#ifndef _WIN32
        require(executable(file) == value.modes.at(name), "installed executable mode has changed");
#endif
    }
}
void verify_fingerprints(const fs::path& path, const Json& manifest, const std::string& bundle_hash) {
    const auto found = inventory(path);
    std::set<std::string> expected{"plugin.json", "bundle.csplugin"};
    for (const auto& record : manifest["files"]) expected.insert(record["path"].get<std::string>());
    require(found == expected && directories(path) == parents(expected), "installed inventory has changed");
    require(read_build_manifest_snapshot(path / "plugin.json", max_metadata) == encoded(manifest), "installed manifest has changed");
    require(sha256_bytes(read_build_manifest_snapshot(path / "bundle.csplugin", plugin_detail::max_bundle)) == bundle_hash,
            "installed original bundle has changed");
#ifndef _WIN32
    require(!executable(path / "plugin.json") && !executable(path / "bundle.csplugin"), "installed metadata modes have changed");
#endif
    for (const auto& record : manifest["files"]) {
        const auto file = path / path_of(record["path"].get<std::string>());
        const auto bytes = read_build_manifest_snapshot(file, max_payload);
        require(bytes.size() == record["size"].get<std::size_t>() && sha256_bytes(bytes) == record["sha256"].get<std::string>(), "installed payload has changed");
#ifndef _WIN32
        require(executable(file) == record["executable"].get<bool>(), "installed executable mode has changed");
#endif
    }
}
Package installed_package(const fs::path& path) {
    const auto raw = read_build_manifest_snapshot(path / "bundle.csplugin", plugin_detail::max_bundle);
    auto result = package(raw, sha256_bytes(raw));
    require(utf8(path.filename()) == result.hash && utf8(path.parent_path().filename()) == result.manifest["version"].get<std::string>() &&
            utf8(path.parent_path().parent_path().filename()) == result.manifest["id"].get<std::string>(), "installed path does not match content identity");
    verify_installation(path, result);
    return result;
}
void write_file(const fs::path& path, const std::string& bytes, bool is_executable) {
    Directory parent(path.parent_path(), true);
    parent.write(utf8(path.filename()), bytes, is_executable);
}
Json install_bytes(const std::string& bytes, const std::string& expected, const fs::path& requested_store,
                   const std::atomic_bool* cancelled = nullptr) {
    const auto value = package(bytes, expected); // All validation precedes store mutation.
    const auto store = checked_absolute(requested_store);
    const auto version_dir = store / path_of(value.manifest["id"].get<std::string>()) / path_of(value.manifest["version"].get<std::string>());
    const auto destination = version_dir / value.hash;
    if (fs::exists(destination)) {
        verify_installation(destination, value);
        auto result = identity(value); result.update({{"status", "unchanged"}, {"directory", utf8(destination)}, {"sha256", value.hash}});
        return result;
    }
    require(!cancelled || !cancelled->load(), "installation cancelled before filesystem mutation");
    Directory parent(version_dir, true);
    MutationLock operation(version_dir, ".plugin-install.lock");
    if (fs::exists(destination)) {
        verify_installation(destination, value);
        auto result = identity(value); result.update({{"status", "unchanged"}, {"directory", utf8(destination)}, {"sha256", value.hash}});
        return result;
    }
    const auto stage_name = ".plugin-stage-" + unique();
    const auto stage = version_dir / stage_name;
    parent.create_directory(stage_name);
    // On write failure retain the owned stage for explicit inspection. It is
    // never treated as a complete installation or silently recursively removed.
    for (const auto& [name, data] : value.files) write_file(stage / path_of(name), data, value.modes.at(name));
    write_file(stage / "plugin.json", encoded(value.manifest), false);
    write_file(stage / "bundle.csplugin", value.bytes, false);
    verify_installation(stage, value);
    parent.publish(stage_name, value.hash, false);
    verify_installation(destination, value);
    auto result = identity(value); result.update({{"status", "installed"}, {"directory", utf8(destination)}, {"sha256", value.hash}});
    return result;
}
void validate_lock(const Json& value) {
    keys(value, {"format", "id", "version", "platform", "arch", "bundle_sha256", "manifest_sha256", "directory"});
    require(value["format"].is_number_integer() && value["format"] == 2, "unsupported lock format");
    require(hex_digest(string(value, "bundle_sha256", 64)) && hex_digest(string(value, "manifest_sha256", 64)), "invalid locked digest");
    const auto id = string(value, "id", 64); require(name(id, 64) && lower(id) == id, "invalid locked id");
    version(string(value, "version", 20));
    const auto [platform, arch] = native();
    require(value["platform"] == platform && value["arch"] == arch, "locked target differs from native platform");
    const auto directory = path_of(string(value, "directory", 32768));
    require(directory.is_absolute() && checked_absolute(directory) == directory, "lock installation path must be absolute and canonical");
}
Json lock_for(const fs::path& installed, const Package& value) {
    auto result = identity(value);
    result["manifest_sha256"] = sha256_bytes(encoded(value.manifest));
    result["directory"] = utf8(installed);
    return result;
}
}

Json pack_plugin(const fs::path& source, const fs::path& output) {
    const auto root = checked_absolute(source), target = checked_absolute(output);
    require(!target.lexically_relative(root).empty() && *target.lexically_relative(root).begin() == "..",
            "bundle output must be outside plugin source");
    const auto raw = read_build_manifest_snapshot(root / "plugin.json", max_metadata);
    auto manifest = parse(raw); descriptor(manifest, false);
    auto names = inventory(root); require(names.erase("plugin.json") == 1 && !names.empty() && names.size() <= max_files, "invalid source inventory");
    require(directories(root) == parents(names), "source has empty or unowned directories");
    Json payloads = Json::object(), files = Json::array();
    std::size_t total = 0;
    for (const auto& name : names) {
        require(lower(name) != "plugin.json" && lower(name) != "bundle.csplugin", "reserved source filename");
        const auto path = root / path_of(name);
        const auto data = read_build_manifest_snapshot(path, max_payload);
        require(data.size() <= max_payload - total, "source exceeds total byte limit"); total += data.size();
        const auto mode = executable(path) || name == manifest["entrypoint"].get<std::string>();
        files.push_back({{"path", name}, {"sha256", sha256_bytes(data)}, {"size", data.size()}, {"executable", mode}});
        payloads[name] = base64(data);
    }
    require(read_build_manifest_snapshot(root / "plugin.json", max_metadata) == raw && inventory(root) == [&] { auto all = names; all.insert("plugin.json"); return all; }(),
            "plugin source changed while packaging");
    for (const auto& record : files) {
        const auto name = record["path"].get<std::string>();
        const auto data = read_build_manifest_snapshot(root / path_of(name), max_payload);
        require(data.size() == record["size"].get<std::size_t>() && sha256_bytes(data) == record["sha256"].get<std::string>(),
                "plugin source changed while packaging");
    }
    manifest["files"] = std::move(files);
    const auto bytes = encoded(Json{{"format", 2}, {"manifest", manifest}, {"payloads", std::move(payloads)}});
    const auto hash = sha256_bytes(bytes);
    const auto value = package(bytes, hash);
    if (fs::exists(target)) require(read_build_manifest_snapshot(target, plugin_detail::max_bundle) == bytes, "bundle output exists with different content");
    else {
        Directory parent(target.parent_path());
        const auto temporary = ".plugin-bundle-" + unique();
        parent.write(temporary, bytes, false); parent.publish(temporary, utf8(target.filename()), false);
    }
    auto result = identity(value); result.update({{"status", "packed"}, {"bundle", utf8(target)}, {"sha256", hash}, {"bytes", bytes.size()}});
    return result;
}
Json install_plugin(const fs::path& bundle, const std::string& expected, const fs::path& store) {
    require(hex_digest(expected), "expected SHA256 is required");
    return install_bytes(read_build_manifest_snapshot(checked_absolute(bundle), plugin_detail::max_bundle), expected, store);
}
Json install_plugin_url(const std::string& url, const std::string& expected, const fs::path& store,
                        const std::atomic_bool& cancelled, bool allow_http_loopback) {
    require(hex_digest(expected), "expected SHA256 is required");
    const auto bytes = plugin_detail::download(url, cancelled, allow_http_loopback);
    require(!cancelled.load(), "installation cancelled before filesystem mutation");
    return install_bytes(bytes, expected, store, &cancelled);
}
Json lock_plugin(const fs::path& installed, const fs::path& output) {
    const auto directory = checked_absolute(installed), target = checked_absolute(output);
    const auto relative = target.lexically_relative(directory);
    require(relative.empty() || *relative.begin() == "..", "lock output must be outside the immutable installation");
    const auto value = installed_package(directory);
    Directory anchored_parent(target.parent_path());
    MutationLock operation(target.parent_path(), "." + utf8(target.filename()) + ".plugin-operation.lock");
    const auto body = encoded(lock_for(directory, value));
    bool unchanged = false, replace = false;
    if (fs::exists(target)) {
        const auto previous = read_build_manifest_snapshot(target, max_metadata);
        validate_lock(parse(previous)); unchanged = previous == body; replace = true;
    }
    if (!unchanged) {
        Directory parent(target.parent_path());
        const auto temporary = ".plugin-lock-" + unique();
        parent.write(temporary, body, false); parent.publish(temporary, utf8(target.filename()), replace);
    }
    auto result = identity(value); result.update({{"status", unchanged ? "unchanged" : "locked"},
        {"lock", utf8(target)}, {"sha256", sha256_bytes(body)}}); return result;
}

struct PluginPlan::Impl {
    fs::path lock, directory, executable;
    std::string snapshot, id, version;
    Package package;
    std::vector<std::string> arguments, tools;
};
PluginPlan::PluginPlan(const fs::path& lock) {
    auto impl = std::make_shared<Impl>();
    impl->lock = checked_absolute(lock);
    impl->snapshot = read_build_manifest_snapshot(impl->lock, max_metadata);
    const auto document = parse(impl->snapshot); validate_lock(document);
    impl->directory = path_of(document["directory"].get<std::string>());
    impl->package = installed_package(impl->directory);
    require(document == lock_for(impl->directory, impl->package), "lock differs from installed content identity");
    impl->id = document["id"].get<std::string>(); impl->version = document["version"].get<std::string>();
    impl->executable = impl->directory / path_of(impl->package.manifest["entrypoint"].get<std::string>());
    for (const auto& item : impl->package.manifest["args"]) {
        const auto value = item.get<std::string>();
        impl->arguments.push_back(value.starts_with("${plugin}/") ? utf8(impl->directory / path_of(value.substr(10))) : value);
    }
    impl->tools = impl->package.manifest["tools"].get<std::vector<std::string>>();
    // Runtime plans retain only the frozen manifest and identity, not dozens of
    // copies of bundle/payload bodies. Later verification hashes one bounded
    // file at a time against this frozen manifest.
    impl->package.bytes.clear(); impl->package.bytes.shrink_to_fit();
    impl->package.files.clear(); impl->package.modes.clear();
    impl_ = std::move(impl);
}
const std::string& PluginPlan::snapshot() const { return impl_->snapshot; }
const std::string& PluginPlan::id() const { return impl_->id; }
const std::string& PluginPlan::version() const { return impl_->version; }
const std::string& PluginPlan::bundle_sha256() const { return impl_->package.hash; }
const fs::path& PluginPlan::directory() const { return impl_->directory; }
const fs::path& PluginPlan::executable() const { return impl_->executable; }
const std::vector<std::string>& PluginPlan::arguments() const { return impl_->arguments; }
const std::vector<std::string>& PluginPlan::tools() const { return impl_->tools; }
std::size_t PluginPlan::retained_bytes() const {
    std::size_t result = impl_->snapshot.size() + encoded(impl_->package.manifest).size();
    for (const auto& argument : impl_->arguments) result += argument.size();
    for (const auto& tool : impl_->tools) result += tool.size();
    return result;
}
std::size_t PluginPlan::payload_bytes() const {
    std::size_t result = 0;
    for (const auto& record : impl_->package.manifest["files"]) result += record["size"].get<std::size_t>();
    return result;
}
void PluginPlan::verify() const { verify_fingerprints(impl_->directory, impl_->package.manifest, impl_->package.hash); }
std::shared_ptr<const ProcessPackage> PluginPlan::process_package() const {
    std::vector<std::string> arguments;
    for (const auto& item:impl_->package.manifest["args"]) {
        auto argument=item.get<std::string>();
        if (argument.starts_with("${plugin}/")) argument="${package}/"+argument.substr(10);
        arguments.push_back(std::move(argument));
    }
    return std::make_shared<ProcessPackage>(impl_->directory,impl_->package.manifest["files"],
        path_of(impl_->package.manifest["entrypoint"].get<std::string>()),arguments);
}
Json inspect_plugin_lock(const fs::path& lock) {
    const PluginPlan plan(lock);
    auto result = parse(plan.snapshot());
    result.update({{"status", "verified"}, {"lock", utf8(checked_absolute(lock))}, {"lock_sha256", sha256_bytes(plan.snapshot())},
        {"entrypoint", utf8(plan.executable())}, {"arguments", plan.arguments()}, {"tools", plan.tools()}});
    return result;
}
}
