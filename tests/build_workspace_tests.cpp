#include "codestudio/build_workspace.hpp"
#include "codestudio/store.hpp"
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <fcntl.h>
#include <membership.h>
#include <pwd.h>
#include <sys/acl.h>
#endif

namespace {
namespace fs = std::filesystem;
void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("build workspace test: " + message);
}
template <typename F> void rejects(F&& operation, const char* message) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
#ifdef _WIN32
struct Temp {
    fs::path root = fs::temp_directory_path() / ("codestudio-build-copy-tests-" + codestudio::new_id());
    Temp() {
        fs::create_directories(root);
        std::wstring expanded(32768,L'\0');
        const auto length = GetLongPathNameW(root.c_str(),expanded.data(),static_cast<DWORD>(expanded.size()));
        check(length && length < expanded.size(), "expand temporary fixture path");
        expanded.resize(length); root = fs::path(expanded);
    }
    ~Temp() { try { codestudio::safe_remove_owned_tree(root); } catch (...) {} }
    fs::path directory(const char* name) { auto path = root / name; fs::create_directories(path); return path; }
};
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file.write(text.data(), static_cast<std::streamsize>(text.size()));
    check(static_cast<bool>(file), "write fixture");
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    check(static_cast<bool>(file), "read fixture");
    return {std::istreambuf_iterator<char>(file), {}};
}
void junction(const fs::path& link, const fs::path& target) {
    check(CreateDirectoryW(link.c_str(), nullptr), "create junction directory");
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "open junction");
    struct Header { DWORD tag; USHORT length, reserved, substitute_offset, substitute_length, print_offset, print_length; };
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
    const auto success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data.data(), static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr);
    CloseHandle(handle); check(success, "create junction reparse point");
}
std::size_t entries(const fs::path& directory) {
    if (!fs::exists(directory)) return 0;
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}));
}
#elif defined(__linux__) || defined(__APPLE__)
struct Temp {
    fs::path root;
    explicit Temp(const fs::path& parent=fs::temp_directory_path())
        : root(parent / ("codestudio-build-copy-tests-" + codestudio::new_id())) {
        fs::create_directory(root); root = fs::canonical(root);
    }
    ~Temp() { try { codestudio::safe_remove_owned_tree(root); } catch (...) {} }
    fs::path directory(const char* name) { auto path = root / name; fs::create_directories(path); return path; }
};
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file.write(text.data(), static_cast<std::streamsize>(text.size()));
    check(static_cast<bool>(file), "write fixture");
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary); check(static_cast<bool>(file), "read fixture");
    return {std::istreambuf_iterator<char>(file), {}};
}
std::size_t entries(const fs::path& directory) {
    if (!fs::exists(directory)) return 0;
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}));
}
mode_t mode(const fs::path& path) {
    struct stat data{}; check(lstat(path.c_str(), &data) == 0, "inspect fixture mode");
    return data.st_mode & 07777;
}
void link_to(const fs::path& link, const fs::path& target) {
    check(symlink(target.c_str(), link.c_str()) == 0, "create symlink fixture");
}
#endif
#ifdef __APPLE__
template <typename F> void rejects_with(F&& operation,const char* expected,const char* message) {
    try { operation(); }
    catch (const std::exception& error) {
        check(std::string(error.what()).find(expected)!=std::string::npos,message);
        return;
    }
    check(false,message);
}
fs::path descriptor_path(const fs::path& path,int command) {
    const auto descriptor=open(path.c_str(),O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    check(descriptor>=0,"open alias fixture");
    char name[PATH_MAX]{};
    const auto result=fcntl(descriptor,command,name);
    close(descriptor);
    check(result==0,"resolve alias fixture descriptor path");
    return fs::path(name);
}
bool has_acl(const fs::path& path) {
    const auto acl=acl_get_file(path.c_str(),ACL_TYPE_EXTENDED);
    if (!acl && errno==ENOENT) {
        struct stat metadata{};
        check(lstat(path.c_str(),&metadata)==0,"ACL fixture entry disappeared");
        return false; // Darwin reports ENOENT for an existing node with no ACL.
    }
    if (!acl) throw std::runtime_error("read fixture ACL: " + path.native() + " (errno " + std::to_string(errno) + ")");
    acl_entry_t entry=nullptr;
    const auto result=acl_get_entry(acl,ACL_FIRST_ENTRY,&entry);
    acl_free(acl);
    return result==0;
}
void inherited_acl(const fs::path& path) {
    auto acl=acl_init(1);
    check(acl!=nullptr,"create fixture ACL");
    struct Release { acl_t& value; ~Release() { acl_free(value); } } release{acl};
    acl_entry_t entry=nullptr;
    acl_permset_t permissions=nullptr;
    acl_flagset_t flags=nullptr;
    uuid_t principal{};
    check(mbr_uid_to_uuid(getuid(),principal)==0 && acl_create_entry(&acl,&entry)==0 &&
          acl_set_tag_type(entry,ACL_EXTENDED_ALLOW)==0 && acl_set_qualifier(entry,principal)==0 &&
          acl_get_permset(entry,&permissions)==0 && acl_add_perm(permissions,ACL_READ_DATA)==0 &&
          acl_get_flagset_np(entry,&flags)==0 && acl_add_flag_np(flags,ACL_ENTRY_FILE_INHERIT)==0 &&
          acl_add_flag_np(flags,ACL_ENTRY_DIRECTORY_INHERIT)==0 &&
          acl_set_file(path.c_str(),ACL_TYPE_EXTENDED,acl)==0,"set inherited fixture ACL");
}
#endif
}

void run_build_workspace_tests() {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    {
        using namespace codestudio;
        Temp fixture;std::atomic_bool cancelled{false};
        const auto protected_parent=fixture.directory("protected");
        const auto nested_source=protected_parent / "child";
        write(nested_source / "visible","private state");
        BuildImportOptions inputs;inputs.workspace_exclusions=false;
        for (const auto& protected_path:std::vector<fs::path>{protected_parent,nested_source}) {
            inputs.protected_paths={protected_path};
            for (const auto& selection:std::vector<fs::path>{"visible","."}) {
                inputs.inputs={selection};
                const auto destination=fixture.directory(("ancestor-rejected-"+new_id()).c_str());
                rejects([&] {import_build_tree(nested_source,destination,inputs,cancelled);},
                        "source inside a protected directory was imported with workspace exclusions disabled");
                check(entries(destination)==0,"protected source ancestor copied data before refusal");
            }
        }
        const auto host=protected_parent / "host";
        fs::create_directory(host);
        const auto artifacts_source=fixture.directory("artifacts-source");
        write(artifacts_source / "visible","artifact");
        for (const auto& protected_path:std::vector<fs::path>{protected_parent,host}) {
            rejects([&] {export_build_artifacts(artifacts_source,{"visible"},host,"artifacts",
                {protected_path},{},cancelled);},"artifact host inside a protected directory was accepted");
            check(!fs::exists(host / "artifacts"),"protected host ancestor created an artifact directory");
        }
    }
#endif
#ifdef _WIN32
    using namespace codestudio;
    std::atomic_bool cancelled{false};
    Temp temp;
    const auto source = temp.directory("source"), target = temp.directory("private");
    const std::string binary{"A\0\xff\x01Z", 5};
    write(source / "src" / "binary.dat", binary);
    write(source / "src" / "text.txt", "abc");
    write(source / ".git" / "config", "private git config");
    write(source / ".codestudio" / "state.sqlite3", "private state");
    write(source / "state.sqlite3", "protected database");
    write(source / "configuration" / "plugins.json", "protected manifest");
    check(read_build_manifest_snapshot(source / "configuration/plugins.json") == "protected manifest", "manifest snapshot bytes");
    rejects([&] { read_build_manifest_snapshot(source / "configuration/plugins.json", 2); }, "oversize manifest snapshot accepted");
    write(source / "cache" / "large.bin", "excluded data");
    BuildImportOptions options;
    options.inputs = {"."}; options.excludes = {"cache"};
    options.protected_paths = {source / "state.sqlite3", "configuration/plugins.json"};
    const auto imported = import_build_tree(source, target, options, cancelled);
    check(imported.files == 2 && imported.bytes == 8, "import counts selected binary bytes");
    const auto measured = measure_build_tree(target, {}, cancelled);
    check(measured.files == 2 && measured.bytes == 8, "strict disk snapshot counts copied bytes");
    check(read(target / "src" / "binary.dat") == binary, "binary copy preserves NUL/high bytes");
    check(!fs::exists(target / ".git") && !fs::exists(target / ".codestudio") && !fs::exists(target / "state.sqlite3") &&
        !fs::exists(target / "configuration" / "plugins.json") && !fs::exists(target / "cache"), "mandatory and explicit exclusions");
    check(read(source / "state.sqlite3") == "protected database", "original database unchanged");
    for (const auto& bad : std::vector<fs::path>{"../outside", "src/../state.sqlite3", "src/text.txt:secret", "NUL", "CON.txt", "COM1.log", "src/space ", ".git", "state.sqlite3", "cache"}) {
        auto invalid = options; invalid.inputs = {bad};
        rejects([&] { import_build_tree(source, temp.directory("invalid"), invalid, cancelled); }, "unsafe/protected explicit import accepted");
    }
    auto single = options; single.inputs = {"src/text.txt"};
    auto budget = single; budget.limits.max_bytes = 2;
    rejects([&] { import_build_tree(source, temp.directory("byte-budget"), budget, cancelled); }, "byte budget ignored");
    budget = single; budget.limits.max_files = 0;
    rejects([&] { import_build_tree(source, temp.directory("file-budget"), budget, cancelled); }, "file count budget ignored");
    budget = single; budget.limits.max_file_bytes = 2;
    rejects([&] { import_build_tree(source, temp.directory("single-file-budget"), budget, cancelled); }, "single file budget ignored");
    rejects([&] { measure_build_tree(target, budget.limits, cancelled); }, "measurement byte limits ignored");
    budget = single; budget.limits.max_depth = 0;
    rejects([&] { import_build_tree(source, temp.directory("depth-budget"), budget, cancelled); }, "depth budget ignored");
    cancelled = true;
    rejects([&] { import_build_tree(source, temp.directory("cancelled"), single, cancelled); }, "cancelled import proceeded");
    cancelled = false;
    auto overlap = options; overlap.inputs = {"src/text.txt", "src", "src/text.txt"};
    check(import_build_tree(source, temp.directory("overlap"), overlap, cancelled).files == 2, "overlapping selections copied twice");
    rejects([&] { import_build_tree(source, source / "src", single, cancelled); }, "nested import accepted");
    const auto toolchain = temp.directory("toolchain");
    write(toolchain / ".git" / "tool.dat", "tool");
    BuildImportOptions tool_options; tool_options.inputs = {"."}; tool_options.workspace_exclusions = false;
    check(import_build_tree(toolchain, temp.directory("private-tools"), tool_options, cancelled).files == 1, "approved toolchain copy applies workspace exclusions");

    const auto host = temp.directory("host");
    const auto published = export_build_artifacts(target, {"src"}, host, "artifacts", {}, {}, cancelled);
    check(published.at("file_count") == 2 && published.at("bytes") == 8, "artifact totals");
    const auto bundle = utf8_path(published.at("directory").get<std::string>());
    check(read(bundle / "files" / "src" / "binary.dat") == binary, "export preserves binary bytes");
    const auto manifest = Json::parse(read(bundle / "manifest.json"));
    check(manifest.at("schema_version") == 2 && manifest == published, "published artifact manifest matches the current receipt format");
    bool hash_checked = false;
    for (const auto& file : manifest.at("files")) if (file.at("path") == "src/text.txt") {
        check(file.at("sha256") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "artifact SHA256");
        check(file.at("artifact_path") == "files/src/text.txt" && file.at("bytes") == 3, "artifact relative mapping"); hash_checked = true;
    }
    check(hash_checked, "text artifact in manifest");
    const auto second = export_build_artifacts(target, {"src/text.txt"}, host, "artifacts", {}, {}, cancelled);
    check(second.at("directory") != published.at("directory") && read(bundle / "files/src/text.txt") == "abc", "new exports never overwrite earlier bundles");
    const auto bundle_count = entries(host / "artifacts");
    BuildCopyLimits small; small.max_bytes = 1;
    rejects([&] { export_build_artifacts(target, {"src"}, host, "artifacts", {}, small, cancelled); }, "over-budget export accepted");
    check(entries(host / "artifacts") == bundle_count, "failed export published or leaked staging bundle");
    BuildCopyLimits tiny_manifest; tiny_manifest.max_manifest_bytes = 64;
    rejects([&] { export_build_artifacts(target, {"src"}, host, "artifacts", {}, tiny_manifest, cancelled); }, "oversize serialized manifest published");
    check(entries(host / "artifacts") == bundle_count, "oversize manifest leaked a bundle");
    rejects([&] { export_build_artifacts(target, {"src/text.txt", "missing"}, host, "artifacts", {}, {}, cancelled); }, "partial output selection published");
    check(entries(host / "artifacts") == bundle_count, "partially copied outputs published or retained");
    for (const auto& destination : std::vector<fs::path>{"../escape", ".codestudio/output", ".git", "artifacts:secret", "NUL", "state"})
        rejects([&] { export_build_artifacts(target, {"src"}, host, destination, {"state"}, {}, cancelled); }, "unsafe/protected export destination accepted");
    fs::create_directory(host / ".codestudio");
    std::wstring short_name(32768, L'\0');
    const auto short_size = GetShortPathNameW((host / ".codestudio").c_str(), short_name.data(), static_cast<DWORD>(short_name.size()));
    if (short_size && short_size < short_name.size()) {
        short_name.resize(short_size);
        const auto alias = fs::path(short_name).filename();
        if (alias != L".codestudio")
            rejects([&] { export_build_artifacts(target, {"src"}, host, alias, {}, {}, cancelled); }, "short-name protected destination alias accepted");
    }
    cancelled = true;
    rejects([&] { export_build_artifacts(target, {"src"}, host, "artifacts", {}, {}, cancelled); }, "cancelled export proceeded");
    cancelled = false;
    check(entries(host / "artifacts") == bundle_count, "cancelled export published artifacts");

    const auto outside = temp.directory("outside"); write(outside / "sentinel", "untouched");
    junction(source / "escape", outside);
    rejects([&] { read_build_manifest_snapshot(source / "escape/sentinel"); }, "manifest junction ancestor accepted");
    auto unsafe = options; unsafe.inputs = {"escape"};
    rejects([&] { import_build_tree(source, temp.directory("link-import"), unsafe, cancelled); }, "junction import accepted");
    unsafe.inputs = {"escape/sentinel"};
    rejects([&] { import_build_tree(source, temp.directory("link-parent-import"), unsafe, cancelled); }, "junction parent import accepted");
    junction(target / "escape", outside);
    rejects([&] { measure_build_tree(target, {}, cancelled); }, "measurement followed a junction");
    rejects([&] { export_build_artifacts(target, {"escape"}, host, "artifacts", {}, {}, cancelled); }, "junction export accepted");
    check(CreateHardLinkW((target / "hardlink").c_str(), (outside / "sentinel").c_str(), nullptr), "create hardlink fixture");
    rejects([&] { read_build_manifest_snapshot(target / "hardlink"); }, "hard-linked manifest accepted");
    BuildImportOptions linked; linked.inputs = {"hardlink"};
    rejects([&] { import_build_tree(target, temp.directory("hardlink-import"), linked, cancelled); }, "hard-linked source bypassed protected path identity");
    rejects([&] { export_build_artifacts(target, {"hardlink"}, host, "artifacts", {}, {}, cancelled); }, "hard-linked artifact accepted");
    check(entries(host / "artifacts") == bundle_count, "unsafe outputs leaked staging or published bundle");
    junction(host / "linked-destination", outside);
    rejects([&] { export_build_artifacts(target, {"src"}, host, "linked-destination", {}, {}, cancelled); }, "junction export parent accepted");
    safe_remove_owned_tree(source / "escape");
    check(read(outside / "sentinel") == "untouched" && !fs::exists(source / "escape"), "cleanup traversed junction target");
    const auto cleanup = temp.directory("cleanup"); junction(cleanup / "escape", outside);
    write(cleanup / "readonly", "owned");
    check(SetFileAttributesW((cleanup / "readonly").c_str(), FILE_ATTRIBUTE_READONLY), "set readonly cleanup fixture");
    safe_remove_owned_tree(cleanup);
    check(!fs::exists(cleanup) && read(outside / "sentinel") == "untouched", "owned tree cleanup escaped or retained ordinary entries");
#elif defined(__linux__) || defined(__APPLE__)
    using namespace codestudio;
    Temp temp; std::atomic_bool cancelled{false};
    const auto source = temp.directory("source"), target = temp.directory("private"), host = temp.directory("host");
    const std::string binary("\0a\0b", 4);
    write(source / "src/text.txt", "abc"); write(source / "src/binary", binary);
    write(source / "src/skip", "excluded"); write(source / "src/protected", "private state");
    write(source / ".git/config", "git secret"); write(source / ".codestudio/state", "agent secret");
    write(source / "script.sh", "exit 0\n");
    check(chmod((source / "script.sh").c_str(), 06755) == 0, "set executable fixture mode");
    BuildImportOptions options; options.inputs = {"src", "script.sh", "src/text.txt"};
    options.excludes = {"src/skip"}; options.protected_paths = {source / "src/protected"};
    const auto imported = import_build_tree(source, target, options, cancelled);
    check(imported.files == 3 && imported.bytes == 14, "selective import accounting or overlapping selections");
    check(read(target / "src/text.txt") == "abc" && read(target / "src/binary") == binary, "import bytes changed");
    check(!fs::exists(target / "src/skip") && !fs::exists(target / "src/protected"), "excluded or protected input copied");
    check(mode(target / "script.sh") == 0700 && mode(target / "src/text.txt") == 0600,
        "import did not retain executable permission while stripping privileged and shared access bits");
    check(read_build_manifest_snapshot(source / "src/text.txt") == "abc", "manifest snapshot differs from opened file");
    rejects([&] { read_build_manifest_snapshot(source / "src/text.txt", 2); }, "oversize manifest snapshot accepted");
    rejects([&] { read_build_manifest_snapshot(source / "src"); }, "directory accepted as manifest");
    rejects([&] { import_build_tree(source, target, options, cancelled); }, "import overwrote existing files");
    check(read(target / "src/text.txt") == "abc", "failed exclusive copy changed existing bytes");

    auto whole = options; whole.inputs = {"."};
    const auto whole_target = temp.directory("whole");
    import_build_tree(source, whole_target, whole, cancelled);
    check(!fs::exists(whole_target / ".git") && !fs::exists(whole_target / ".codestudio"), "reserved workspace state copied");
    auto explicit_inputs = options;
    for (const auto& selection : std::vector<fs::path>{"src/missing", "src/protected", "src/skip", ".git/config", "../source"}) {
        const auto rejected_target = temp.directory(("invalid-" + new_id()).c_str());
        explicit_inputs.inputs = {"src", selection};
        rejects([&] { import_build_tree(source, rejected_target, explicit_inputs, cancelled); },
            "overlapping selections hid an explicit missing, protected, or invalid input");
        check(entries(rejected_target) == 0, "selection validation copied bytes before rejecting input");
    }
    rejects([&] { import_build_tree(source, source, options, cancelled); }, "import accepted identical roots");
    rejects([&] { import_build_tree(source, source / "src", options, cancelled); }, "import accepted overlapping roots");
    auto empty = options; empty.inputs.clear();
    rejects([&] { import_build_tree(source, temp.directory("empty-input"), empty, cancelled); }, "empty input selection accepted");
    for (int kind = 0; kind < 5; ++kind) {
        BuildImportOptions limited; limited.inputs = {"src/text.txt"};
        if (kind == 0) limited.limits.max_files = 0;
        if (kind == 1) limited.limits.max_bytes = 2;
        if (kind == 2) limited.limits.max_file_bytes = 2;
        if (kind == 3) limited.limits.max_depth = 1;
        if (kind == 4) limited.limits.max_entries = 1;
        rejects([&] { import_build_tree(source, temp.directory(("budget-" + std::to_string(kind)).c_str()), limited, cancelled); },
            "import budget was not enforced");
    }
    cancelled = true;
    rejects([&] { import_build_tree(source, temp.directory("cancelled"), options, cancelled); }, "cancelled import proceeded");
    rejects([&] { measure_build_tree(target, {}, cancelled); }, "cancelled measurement proceeded");
    cancelled = false;
    const auto measured = measure_build_tree(target, {}, cancelled);
    check(measured.files == imported.files && measured.bytes == imported.bytes, "measurement disagrees with copied tree");
    BuildCopyLimits tiny; tiny.max_bytes = 1;
    rejects([&] { measure_build_tree(target, tiny, cancelled); }, "measurement byte budget ignored");
    tiny = {}; tiny.max_depth = 0;
    rejects([&] { measure_build_tree(target, tiny, cancelled); }, "measurement depth budget ignored");
    tiny = {}; tiny.max_entries = 1;
    rejects([&] { measure_build_tree(target, tiny, cancelled); }, "measurement entry budget ignored");

    const auto published = export_build_artifacts(target, {"src", "script.sh"}, host, "artifacts", {}, {}, cancelled);
    const auto bundle = fs::path(published.at("directory").get<std::string>());
    const auto manifest = Json::parse(read(bundle / "manifest.json"));
    check(manifest == published && manifest.at("schema_version") == 2 && manifest.at("file_count") == 3 &&
        manifest.at("bytes") == 14, "artifact receipt differs from committed manifest or byte accounting");
    bool hash_checked = false;
    for (const auto& file : manifest.at("files")) if (file.at("path") == "src/text.txt") {
        check(file.at("sha256") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
            file.at("artifact_path") == "files/src/text.txt" && file.at("bytes") == 3, "artifact hash or mapping changed");
        hash_checked = true;
    }
    check(hash_checked && read(bundle / "files/src/binary") == binary && mode(bundle / "files/script.sh") == 0700,
        "export changed binary bytes or executable permission");
    check(read(source / "src/text.txt") == "abc" && read(target / "src/text.txt") == "abc", "export changed source files");
    const auto second = export_build_artifacts(target, {"src/text.txt"}, host, "artifacts", {}, {}, cancelled);
    check(second.at("directory") != published.at("directory"), "export reused an existing bundle");
    const auto bundle_count = entries(host / "artifacts");
    tiny = {}; tiny.max_bytes = 1;
    rejects([&] { export_build_artifacts(target, {"src"}, host, "artifacts", {}, tiny, cancelled); }, "over-budget export accepted");
    tiny = {}; tiny.max_manifest_bytes = 64;
    rejects([&] { export_build_artifacts(target, {"src"}, host, "artifacts", {}, tiny, cancelled); }, "oversize export manifest accepted");
    rejects([&] { export_build_artifacts(target, {"src", "src/missing"}, host, "artifacts", {}, {}, cancelled); },
        "overlapping output selection hid a missing path");
    for (const auto& destination : std::vector<fs::path>{"../escape", ".", ".git", ".codestudio/out", "state"})
        rejects([&] { export_build_artifacts(target, {"src"}, host, destination, {"state"}, {}, cancelled); },
            "unsafe or protected export destination accepted");
    for (const auto& output : std::vector<fs::path>{"../escape", ".codestudio/state", ".git/config"})
        rejects([&] { export_build_artifacts(target, {output}, host, "artifacts", {}, {}, cancelled); }, "unsafe output accepted");
    cancelled = true;
    rejects([&] { export_build_artifacts(target, {"src"}, host, "artifacts", {}, {}, cancelled); }, "cancelled export proceeded");
    cancelled = false;
    check(entries(host / "artifacts") == bundle_count, "failed export leaked staging or published a bundle");

    const auto outside = temp.directory("outside"); write(outside / "sentinel", "untouched");
    check(chmod((outside / "sentinel").c_str(), 0440) == 0, "set outside sentinel mode");
    const auto outside_mode = mode(outside / "sentinel");
    for (const auto* kind : {"symlink", "hardlink", "fifo"}) {
        const auto unsafe = temp.directory(kind), path = unsafe / "input";
        if (std::string(kind) == "symlink") link_to(path, outside / "sentinel");
        else if (std::string(kind) == "hardlink") check(link((outside / "sentinel").c_str(), path.c_str()) == 0, "create hardlink fixture");
        else check(mkfifo(path.c_str(), 0600) == 0, "create FIFO fixture");
        BuildImportOptions selected; selected.inputs = {"input"};
        rejects([&] { read_build_manifest_snapshot(path); }, "nonregular or hard-linked manifest accepted");
        rejects([&] { import_build_tree(unsafe, temp.directory((std::string(kind) + "-target").c_str()), selected, cancelled); },
            "nonregular or hard-linked input accepted");
        rejects([&] { measure_build_tree(unsafe, {}, cancelled); }, "measurement accepted unsafe file type");
        rejects([&] { export_build_artifacts(unsafe, {"input"}, host, "artifacts", {}, {}, cancelled); }, "unsafe artifact accepted");
        safe_remove_owned_tree(unsafe);
        check(read(outside / "sentinel") == "untouched" && mode(outside / "sentinel") == outside_mode,
            "cleanup changed an external linked file or its permissions");
    }
    const auto alias = temp.root / "alias"; link_to(alias, outside);
    rejects([&] { read_build_manifest_snapshot(alias / "sentinel"); }, "manifest followed a symlink ancestor");
    BuildImportOptions selected; selected.inputs = {"sentinel"};
    rejects([&] { import_build_tree(alias, temp.directory("alias-target"), selected, cancelled); }, "import followed root symlink");
    rejects([&] { import_build_tree(source, alias, options, cancelled); }, "import followed destination symlink");
    rejects([&] { safe_remove_owned_tree(alias / "sentinel"); }, "cleanup followed a symlink ancestor");
    link_to(host / "linked-destination", outside);
    rejects([&] { export_build_artifacts(target, {"src"}, host, "linked-destination", {}, {}, cancelled); },
        "export followed destination symlink");
    check(entries(host / "artifacts") == bundle_count, "unsafe export leaked staging or published a bundle");
    safe_remove_owned_tree(alias);
    check(!fs::exists(fs::symlink_status(alias)) && read(outside / "sentinel") == "untouched", "cleanup traversed symlink target");
    const auto cleanup = temp.directory("cleanup"); link_to(cleanup / "escape", outside);
    write(cleanup / "readonly", "owned"); check(chmod((cleanup / "readonly").c_str(), 0400) == 0, "set readonly fixture mode");
    safe_remove_owned_tree(cleanup); safe_remove_owned_tree(cleanup);
    check(!fs::exists(cleanup) && read(outside / "sentinel") == "untouched" && mode(outside / "sentinel") == outside_mode,
        "recursive cleanup escaped or changed external permissions");
    rejects([&] { safe_remove_owned_tree("/"); }, "cleanup accepted filesystem root");
#ifdef __APPLE__
    {
    using namespace codestudio;
    struct SnapshotFixture {
        fs::path root;
        SnapshotFixture() {
            root = fs::temp_directory_path() / ("codestudio-manifest-tests-" + new_id());
            fs::create_directory(root); root = fs::canonical(root);
        }
        ~SnapshotFixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    } fixture;
    const auto file = fixture.root / "manifest.json";
    const std::string bytes{"A\0\xff\x01Z", 5};
    { std::ofstream output(file, std::ios::binary); output.write(bytes.data(), bytes.size()); }
    check(read_build_manifest_snapshot(file) == bytes, "snapshot did not preserve raw bytes");
    check(read_build_manifest_snapshot(file, bytes.size()) == bytes, "exact byte limit rejected");
    rejects([&] { read_build_manifest_snapshot(file, bytes.size() - 1); }, "oversize snapshot accepted");
    rejects([&] { read_build_manifest_snapshot(fixture.root); }, "directory accepted as a manifest");
    rejects([&] { read_build_manifest_snapshot("manifest.json"); }, "relative manifest accepted");
    rejects([&] { read_build_manifest_snapshot(fixture.root / ".." / fixture.root.filename() / file.filename()); },
            "parent traversal accepted");
    const auto link = fixture.root / "linked.json";
    fs::create_symlink(file, link);
    rejects([&] { read_build_manifest_snapshot(link); }, "symlink manifest followed");
    const auto alias = fixture.root / "alias";
    fs::create_directory_symlink(fixture.root, alias);
    rejects([&] { read_build_manifest_snapshot(alias / file.filename()); }, "symlink ancestor followed");
    const auto hardlink = fixture.root / "hardlink.json";
    fs::create_hard_link(file, hardlink);
    rejects([&] { read_build_manifest_snapshot(file); }, "hard-linked source accepted");
    rejects([&] { read_build_manifest_snapshot(hardlink); }, "hard-linked manifest accepted");
    fs::remove(hardlink);
    const auto fifo = fixture.root / "fifo.json";
    check(mkfifo(fifo.c_str(), 0600) == 0, "create FIFO fixture");
    rejects([&] { read_build_manifest_snapshot(fifo); }, "FIFO accepted as a manifest");
    check(read_build_manifest_snapshot(file) == bytes, "failed reads changed the source");
    }
    // Policy comparisons must catch aliases even when the protected name or
    // output directory does not exist yet. Conservative rejection also holds
    // on a case-sensitive filesystem.
    {
        const auto aliases_source=temp.directory("aliases-source");
        write(aliases_source / ".CoDeStUdIo/state", "private application state");
        write(aliases_source / ".GiT/config", "private repository state");
        write(aliases_source / "visible", "public");
        const auto aliases_target=temp.directory("aliases-target");
        BuildImportOptions inputs;inputs.inputs={"."};
        import_build_tree(aliases_source,aliases_target,inputs,cancelled);
        check(entries(aliases_target)==1 && read(aliases_target / "visible")=="public",
              "case aliases of reserved input state were copied");
        for (const auto& path:std::vector<fs::path>{".CoDeStUdIo/state",".GiT/config"}) {
            inputs.inputs={path};
            rejects([&] {import_build_tree(aliases_source,temp.directory(("reserved-alias-"+new_id()).c_str()),inputs,cancelled);},
                    "explicit case alias of reserved state was copied");
        }
        const auto unicode_source=temp.directory("unicode-source");
        const fs::path composed="\xc3\xa9-state",decomposed="e\xcc\x81-state";
        write(unicode_source / composed / "secret", "private Unicode state");
        write(unicode_source / "kept", "ordinary");
        inputs.inputs={"."};inputs.protected_paths={unicode_source / decomposed};
        const auto unicode_target=temp.directory("unicode-target");
        import_build_tree(unicode_source,unicode_target,inputs,cancelled);
        check(entries(unicode_target)==1 && read(unicode_target / "kept")=="ordinary",
              "Unicode alias of protected input was copied");
        inputs.inputs={composed / "secret"};
        rejects([&] {import_build_tree(unicode_source,temp.directory("unicode-explicit"),inputs,cancelled);},
                "explicit Unicode alias bypassed protected paths");
        for (const auto& destination:std::vector<fs::path>{".CoDeStUdIo/new", ".GiT/new", composed / "new"}) {
            rejects([&] {export_build_artifacts(target,{"src"},host,destination,{host/decomposed},{},cancelled);},
                    "absent protected artifact directory alias was created");
        }
        check(!fs::exists(host / composed) && !fs::exists(host / ".CoDeStUdIo"),
              "rejected alias export created its destination");
        inputs.protected_paths.clear();inputs.inputs={"kept","KEPT"};
        rejects([&] {import_build_tree(unicode_source,temp.directory("case-selections"),inputs,cancelled);},
                "aliased declared input was silently deduplicated");
        inputs.inputs={composed,decomposed / "secret"};
        rejects([&] {import_build_tree(unicode_source,temp.directory("unicode-selections"),inputs,cancelled);},
                "aliased input prefix was silently deduplicated");
        fs::path invalid(std::string("invalid-")+static_cast<char>(0xff));
        inputs.inputs={invalid};
        rejects([&] {import_build_tree(unicode_source,temp.directory("invalid-utf8"),inputs,cancelled);},
                "invalid UTF-8 path was accepted");
        for (const auto& paths:std::vector<std::pair<fs::path,fs::path>>{
                {unicode_source / composed,unicode_source / decomposed},
                {aliases_source / ".CoDeStUdIo",aliases_source / ".codestudio"}}) {
            const auto child=paths.first / "child";
            write(child / "visible","private aliased state");
            BuildImportOptions protected_inputs;protected_inputs.workspace_exclusions=false;
            protected_inputs.protected_paths={paths.second};
            for (const auto& root:std::vector<fs::path>{paths.first,child}) {
                protected_inputs.inputs={(child / "visible").lexically_relative(root)};
                const auto destination=temp.directory(("aliased-ancestor-"+new_id()).c_str());
                rejects([&] {import_build_tree(root,destination,protected_inputs,cancelled);},
                        "case or Unicode alias of a protected ancestor was ignored");
                check(entries(destination)==0,"aliased protected ancestor copied input bytes");
            }
            rejects([&] {export_build_artifacts(target,{"src"},child,"artifacts",{paths.second},{},cancelled);},
                    "case or Unicode alias of a protected host ancestor was ignored");
            check(!fs::exists(child / "artifacts"),"aliased protected host ancestor created artifacts");
        }
        const auto escaped_source=temp.directory("escaped-source");
        write(escaped_source / "with spaces & \"quotes\"" / "\xe6\x96\x87\xe4\xbb\xb6", "Unicode bytes");
        const auto escaped_target=temp.directory("escaped-target");
        inputs.inputs={"."};
        import_build_tree(escaped_source,escaped_target,inputs,cancelled);
        const auto artifact=export_build_artifacts(escaped_target,{"."},host,"Unicode artifacts & spaces",{},{},cancelled);
        const auto artifact_file=fs::path(artifact.at("directory").get<std::string>()) / "files" / "with spaces & \"quotes\"" / "\xe6\x96\x87\xe4\xbb\xb6";
        check(read(artifact_file)=="Unicode bytes", "ordinary Unicode artifact paths were changed");
    }
    {
        const auto* account=getpwuid(getuid());
        check(account!=nullptr && account->pw_dir!=nullptr,"resolve user home for firmlink fixture");
        Temp home_fixture(fs::canonical(account->pw_dir));
        const auto logical=descriptor_path(home_fixture.root,F_GETPATH);
        const auto physical=descriptor_path(home_fixture.root,F_GETPATH_NOFIRMLINK);
        check(fs::equivalent(logical,physical),"firmlink fixture roots have different identities");
        // User homes on the system Data volume have distinct /Users and
        // physical paths; custom external home volumes may have no firmlink.
        if (logical!=physical) {
            const auto original=home_fixture.directory("source");
            const auto logical_source=logical / "source",physical_source=physical / "source";
            const auto logical_host=logical / "host",physical_host=physical / "host";
            home_fixture.directory("host");
            write(original / "state/secret","private state");
            write(original / "visible","ordinary");
            BuildImportOptions inputs;inputs.inputs={"."};inputs.protected_paths={physical_source / "state"};
            const auto imported=home_fixture.directory("copied");
            check(import_build_tree(logical_source,imported,inputs,cancelled).files==1 &&
                  !fs::exists(imported / "state") && read(imported / "visible")=="ordinary",
                  "physical firmlink protection bypassed logical workspace root");
            inputs.protected_paths={logical_source / "state"};
            const auto reverse=home_fixture.directory("reverse");
            check(import_build_tree(physical_source,reverse,inputs,cancelled).files==1 && !fs::exists(reverse / "state"),
                  "logical firmlink protection bypassed physical workspace root");
            for (const auto& roots:std::vector<std::pair<fs::path,fs::path>>{
                    {logical_source,physical},{physical_source,logical},{logical_source,physical_source}}) {
                inputs.inputs={"visible"};inputs.protected_paths={roots.second};
                const auto destination=home_fixture.directory(("protected-ancestor-"+new_id()).c_str());
                rejects([&] {import_build_tree(roots.first,destination,inputs,cancelled);},
                        "firmlink protected ancestor or same-root alias did not cover the entire source");
                check(entries(destination)==0,"firmlink protected ancestor copied a file");
            }
            for (const auto& roots:std::vector<std::pair<fs::path,fs::path>>{
                    {logical_host,physical_host},{physical_host,logical_host}}) {
                rejects([&] {export_build_artifacts(imported,{"visible"},roots.first,"future-state/artifacts",
                    {roots.second / "future-state"},{},cancelled);},
                    "nonexistent protected suffix bypassed a firmlink root alias");
                check(!fs::exists(roots.first / "future-state"),"rejected firmlink protection created its destination");
            }
            fs::create_directory(original / "nested");write(original / "nested/file","nested");
            inputs.inputs={"visible"};inputs.protected_paths.clear();
            rejects([&] {import_build_tree(logical_source,physical_source,inputs,cancelled);},
                    "same root with a different firmlink spelling was accepted");
            rejects([&] {import_build_tree(logical_source,physical_source / "nested",inputs,cancelled);},
                    "nested destination with a different firmlink spelling was accepted");
            inputs.inputs={"file"};
            rejects([&] {import_build_tree(physical_source / "nested",logical_source,inputs,cancelled);},
                    "ancestor destination with a different firmlink spelling was accepted");
            rejects([&] {export_build_artifacts(logical_source,{"visible"},physical_source,"artifacts",{},{},cancelled);},
                    "export accepted identical roots through firmlink aliases");
            rejects([&] {export_build_artifacts(logical_source,{"visible"},physical_source / "nested","artifacts",{},{},cancelled);},
                    "export accepted a nested host through a firmlink alias");
            rejects([&] {export_build_artifacts(physical_source / "nested",{"file"},logical_source,"artifacts",{},{},cancelled);},
                    "export accepted an ancestor host through a firmlink alias");
            check(!fs::exists(original / "nested/visible") && !fs::exists(original / "file") &&
                  !fs::exists(original / "artifacts") && !fs::exists(original / "nested/artifacts"),
                  "rejected firmlink root overlap wrote files");
            const auto symlink=home_fixture.root / "source-link";
            link_to(symlink,original);
            inputs.inputs={"visible"};inputs.protected_paths={symlink / "state"};
            const auto rejected=home_fixture.directory("protected-symlink");
            rejects([&] {import_build_tree(logical_source,rejected,inputs,cancelled);},
                    "ambiguous protected symlink ancestor was silently ignored");
            check(entries(rejected)==0,"ambiguous protected ancestor copied data before refusal");

            // Resolve the implicit volume prefix without hardcoding it. With
            // the standard system home this is /System/Volumes/Data, which is
            // absent from a logical /Users anchor chain.
            auto volume=physical;
            for (const auto& ignored:logical.relative_path()) { (void)ignored; volume=volume.parent_path(); }
            if (volume!=volume.root_path() && volume / logical.relative_path()==physical) {
                const auto leaf=(physical_source / "visible").lexically_relative(volume);
                const auto artifact_parent=(physical_host / "implicit-artifacts").lexically_relative(volume);
                BuildImportOptions bounded;bounded.inputs={leaf};
                const auto bounded_target=home_fixture.directory("implicit-target");
                rejects_with([&] {import_build_tree(volume,logical / "implicit-target",bounded,cancelled);},
                    "roots overlap","implicit physical ancestor allowed a bounded leaf import into itself");
                check(entries(bounded_target)==0,"implicit ancestor import copied its selected leaf");
                rejects_with([&] {export_build_artifacts(volume,{leaf},logical_host,"implicit-artifacts",{},{},cancelled);},
                    "roots overlap","export missed an implicit physical source ancestor");
                rejects_with([&] {export_build_artifacts(logical_source,{"visible"},volume,artifact_parent,{},{},cancelled);},
                    "roots overlap","export missed an implicit physical host ancestor");
                const auto protected_destination=(physical_host / "future-state/artifacts").lexically_relative(volume);
                rejects_with([&] {export_build_artifacts(logical_source,{"visible"},volume,protected_destination,
                    {logical_host / "future-state"},{},cancelled);},"protected artifact destination",
                    "implicit physical ancestor lost an absent protected suffix");
                rejects_with([&] {export_build_artifacts(imported,{"visible"},logical_host,"ancestor-protected",
                    {volume},{},cancelled);},"protected artifact destination",
                    "implicit physical protected ancestor did not cover the entire host");
                check(!fs::exists(logical_host / "implicit-artifacts") && !fs::exists(logical_host / "future-state") &&
                      !fs::exists(logical_host / "ancestor-protected"),"implicit-ancestor refusal created artifacts");
            }
        }
    }
    {
        const auto acl_target=temp.directory("acl-target");
        inherited_acl(acl_target);
        write(acl_target / "inherited", "fixture");
        check(has_acl(acl_target / "inherited"),"fixture did not inherit the parent ACL");
        fs::remove(acl_target / "inherited");
        BuildImportOptions inputs;inputs.inputs={"src/text.txt"};
        import_build_tree(source,acl_target,inputs,cancelled);
        check(has_acl(acl_target) && !has_acl(acl_target / "src") && !has_acl(acl_target / "src/text.txt"),
              "copy retained inherited ACLs or changed an existing destination ACL");
        const auto acl_artifacts=host / "acl-artifacts";
        fs::create_directory(acl_artifacts);inherited_acl(acl_artifacts);
        const auto artifact=export_build_artifacts(acl_target,{"src"},host,"acl-artifacts",{},{},cancelled);
        const auto directory=fs::path(artifact.at("directory").get<std::string>());
        check(has_acl(acl_artifacts) && !has_acl(directory) && !has_acl(directory / "manifest.json") &&
              !has_acl(directory / "files") && !has_acl(directory / "files/src/text.txt"),
              "published artifact inherited access from its host parent");
    }
#endif
#else
    std::atomic_bool cancelled{false};
    rejects([&] { codestudio::import_build_tree("/source", "/destination", {}, cancelled); }, "unsupported platform must fail closed");
#endif
}
