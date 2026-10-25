#include "codestudio/build_workspace.hpp"
#include "codestudio/store.hpp"
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
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
#elif defined(__linux__)
struct Temp {
    fs::path root = fs::temp_directory_path() / ("codestudio-build-copy-tests-" + codestudio::new_id());
    Temp() { fs::create_directory(root); root = fs::canonical(root); }
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
}

void run_build_workspace_tests() {
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
#elif defined(__linux__)
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
#else
    std::atomic_bool cancelled{false};
    rejects([&] { codestudio::import_build_tree("/source", "/destination", {}, cancelled); }, "unsupported platform must fail closed");
#endif
}
