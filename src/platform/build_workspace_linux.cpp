#ifdef __linux__
#include "codestudio/build_workspace.hpp"
#include "codestudio/store.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <openssl/evp.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace codestudio {
namespace {
namespace fs = std::filesystem;
[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("build workspace: " + message); }
[[noreturn]] void posix_fail(const char* operation) {
    const auto error = errno;
    fail(std::string(operation) + " (errno " + std::to_string(error) + ")");
}
class Fd {
public:
    int value = -1;
    explicit Fd(int descriptor = -1) : value(descriptor) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (value >= 0) close(value); value = std::exchange(other.value, -1); }
        return *this;
    }
};
Fd duplicate(int descriptor) {
    const auto result = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if (result < 0) posix_fail("duplicate directory descriptor");
    return Fd(result);
}
struct stat metadata(int descriptor) {
    struct stat result{};
    if (fstat(descriptor, &result) != 0) posix_fail("inspect open file");
    return result;
}
struct stat entry(int parent, const std::string& name) {
    struct stat result{};
    if (fstatat(parent, name.c_str(), &result, AT_SYMLINK_NOFOLLOW) != 0) posix_fail("inspect directory entry");
    return result;
}
bool same_inode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
        (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}
void verify_entry(int parent, const std::string& name, int descriptor) {
    if (!same_inode(entry(parent,name),metadata(descriptor))) fail("directory entry changed during operation");
}
void component(const fs::path& path) {
    const auto name = path.native();
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\0') != std::string::npos) fail("invalid path component");
}
fs::path checked_absolute(const fs::path& path) {
    if (!path.is_absolute()) fail("expected an absolute path");
    for (const auto& part : path.relative_path()) component(part);
    return path;
}
fs::path checked_relative(const fs::path& path, bool allow_dot = true) {
    if (path.empty() || path.has_root_path()) fail("expected an explicit relative path");
    if (allow_dot && path == ".") return {};
    for (const auto& part : path) component(part);
    return path;
}
bool under(const fs::path& path, const fs::path& prefix) {
    auto item = path.begin();
    for (const auto& part : prefix) { if (item == path.end() || *item != part) return false; ++item; }
    return true;
}
Fd directory_at(int parent, const std::string& name, bool create = false) {
    if (create && mkdirat(parent,name.c_str(),0700) != 0 && errno != EEXIST) posix_fail("create directory");
    Fd result(openat(parent,name.c_str(),O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (result.value < 0) posix_fail("open anchored directory");
    verify_entry(parent,name,result.value);
    return result;
}
// Every path component is opened independently with O_NOFOLLOW. Descriptors
// remain open until the operation finishes; no subsequent traversal starts
// from a mutable full pathname. Revalidation detects renamed/replaced anchors.
struct Anchor {
    std::vector<Fd> descriptors;
    std::vector<std::string> names;
    explicit Anchor(const fs::path& path) {
        checked_absolute(path);
        Fd root(open("/",O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (root.value < 0) posix_fail("open filesystem root");
        descriptors.push_back(std::move(root));
        for (const auto& part : path.relative_path()) append(part.native(),false);
    }
    void append(const std::string& name, bool create) {
        auto child = directory_at(fd(),name,create);
        names.push_back(name); descriptors.push_back(std::move(child));
    }
    int fd() const { return descriptors.back().value; }
    void verify() const {
        for (std::size_t i = 0; i < names.size(); ++i)
            verify_entry(descriptors[i].value,names[i],descriptors[i+1].value);
    }
};
struct DirectoryReader {
    DIR* stream = nullptr;
    explicit DirectoryReader(int descriptor) {
        // A new open-file description has an independent enumeration cursor.
        const auto fd = openat(descriptor,".",O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) posix_fail("open directory enumeration");
        stream = fdopendir(fd);
        if (!stream) { const auto error = errno; close(fd); errno = error; posix_fail("enumerate directory"); }
    }
    ~DirectoryReader() { if (stream) closedir(stream); }
    bool next(std::string& name) {
        for (;;) {
            errno = 0;
            const auto* item = readdir(stream);
            if (!item) { if (errno) posix_fail("read directory entry"); return false; }
            name = item->d_name;
            if (name != "." && name != "..") { component(name); return true; }
        }
    }
};
bool reserved(const fs::path& path) {
    for (const auto& part : path) if (part == ".git" || part == ".codestudio") return true;
    return false;
}
bool matches(const fs::path& path, const std::vector<fs::path>& prefixes) {
    return std::ranges::any_of(prefixes,[&](const auto& prefix) { return prefix.empty() || under(path,prefix); });
}
std::vector<fs::path> protections(const fs::path& root, const std::vector<fs::path>& values) {
    std::vector<fs::path> result;
    for (const auto& value : values) {
        const auto path = value.is_absolute() ? checked_absolute(value) : root / checked_relative(value);
        // A source rooted inside protected state must not bypass its ancestor's policy.
        if (under(root,path)) {
            result.emplace_back();
            continue;
        }
        if (!under(path,root)) continue;
        auto cursor = path.begin();
        for (const auto& ignored : root) { (void)ignored; ++cursor; }
        fs::path suffix;
        for (; cursor != path.end(); ++cursor) suffix /= *cursor;
        result.push_back(std::move(suffix));
    }
    return result;
}
std::vector<fs::path> selections(const std::vector<fs::path>& values) {
    if (values.empty()) fail("at least one explicit input/output path is required");
    std::vector<fs::path> result;
    for (const auto& value : values) {
        auto path = checked_relative(value);
        if (std::ranges::any_of(result,[&](const auto& prior) { return prior.empty() || under(path,prior); })) continue;
        std::erase_if(result,[&](const auto& prior) { return path.empty() || under(prior,path); });
        result.push_back(std::move(path));
    }
    return result;
}
void regular(const struct stat& value) {
    if (!S_ISREG(value.st_mode) || value.st_nlink != 1 || value.st_size < 0)
        fail("non-regular or hard-linked file rejected");
}
bool unchanged(const struct stat& before, const struct stat& after) {
    return same_inode(before,after) && before.st_size == after.st_size && before.st_nlink == after.st_nlink &&
        before.st_mode == after.st_mode && before.st_uid == after.st_uid && before.st_gid == after.st_gid &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}
class Digest {
    EVP_MD_CTX* context = nullptr;
public:
    Digest() : context(EVP_MD_CTX_new()) {
        if (!context || EVP_DigestInit_ex(context,EVP_sha256(),nullptr) != 1) {
            EVP_MD_CTX_free(context); context = nullptr; fail("initialize artifact SHA256");
        }
    }
    ~Digest() { EVP_MD_CTX_free(context); }
    void add(const void* bytes,std::size_t count) {
        if (EVP_DigestUpdate(context,bytes,count) != 1) fail("update artifact SHA256");
    }
    std::string finish() {
        std::array<unsigned char,EVP_MAX_MD_SIZE> bytes{}; unsigned count = 0;
        if (EVP_DigestFinal_ex(context,bytes.data(),&count) != 1 || count != 32) fail("finish artifact SHA256");
        constexpr char digits[] = "0123456789abcdef";
        std::string result; result.reserve(64);
        for (unsigned index = 0; index < count; ++index) { result += digits[bytes[index] >> 4]; result += digits[bytes[index] & 15]; }
        return result;
    }
};
void write_all(int fd,const char* data,std::size_t count) {
    while (count) {
        const auto written = write(fd,data,count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) posix_fail("write private file");
        data += written; count -= static_cast<std::size_t>(written);
    }
}
void sync_file(int fd) { if (fsync(fd) != 0) posix_fail("flush file"); }
void tick(const std::atomic_bool& cancelled) { if (cancelled.load()) fail("cancelled"); }
void budget(const BuildCopyLimits& limits,const BuildCopyStats& stats,std::uint64_t size) {
    if (stats.files >= limits.max_files || size > limits.max_file_bytes || stats.bytes > limits.max_bytes ||
        size > limits.max_bytes - stats.bytes) fail("file/byte copy or measurement budget exceeded");
}
struct Copier {
    int source, destination;
    BuildCopyLimits limits;
    const std::atomic_bool& cancelled;
    std::vector<fs::path> excluded, protected_paths;
    bool workspace_exclusions = true, exporting = false;
    BuildCopyStats stats;
    std::size_t entries = 0;
    Json files = Json::array();
    bool excluded_path(const fs::path& path) const {
        return (workspace_exclusions && reserved(path)) || matches(path,excluded) || matches(path,protected_paths);
    }
    void file(int from,const std::string& name,int to,const fs::path& relative,const struct stat& named) {
        regular(named);
        Fd input(openat(from,name.c_str(),O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        if (input.value < 0) posix_fail("open source file");
        const auto before = metadata(input.value); regular(before);
        if (!same_inode(named,before)) fail("source file changed before opening");
        const auto size = static_cast<std::uint64_t>(before.st_size);
        budget(limits,stats,size);
        Fd output(openat(to,name.c_str(),O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600));
        if (output.value < 0) posix_fail("create exclusive copied file");
        Digest digest;
        std::array<char,65536> bytes{};
        std::uint64_t total = 0;
        for (;;) {
            tick(cancelled);
            const auto count = read(input.value,bytes.data(),bytes.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) posix_fail("read source file");
            if (!count) break;
            if (static_cast<std::uint64_t>(count) > size-total) fail("source grew while copying");
            write_all(output.value,bytes.data(),static_cast<std::size_t>(count));
            if (exporting) digest.add(bytes.data(),static_cast<std::size_t>(count));
            total += static_cast<std::uint64_t>(count);
        }
        if (total != size || !unchanged(before,metadata(input.value))) fail("source changed while copying");
        verify_entry(from,name,input.value);
        // Retain executability only. Never copy setuid/setgid, arbitrary modes,
        // ACLs, capabilities or extended attributes from untrusted input.
        if (fchmod(output.value,(before.st_mode & 0111) ? 0700 : 0600) != 0) posix_fail("set copied file permissions");
        if (exporting) {
            sync_file(output.value);
            const auto path = relative.generic_string();
            files.push_back({{"path",path},{"artifact_path","files/"+path},{"bytes",size},{"sha256",digest.finish()}});
        }
        ++stats.files; stats.bytes += size;
    }
    void directory(int from,int to,const fs::path& relative,std::size_t depth) {
        DirectoryReader reader(from); std::string name;
        while (reader.next(name)) node(from,name,to,relative/name,depth+1,false);
        if (exporting) sync_file(to);
    }
    void node(int from,const std::string& name,int to,const fs::path& relative,std::size_t depth,bool explicit_selection) {
        tick(cancelled);
        if (depth > limits.max_depth || ++entries > limits.max_entries) fail("directory depth/entry copy budget exceeded");
        if (excluded_path(relative)) {
            if (explicit_selection || exporting) fail("protected or excluded path selected");
            return;
        }
        const auto named = entry(from,name);
        if (S_ISDIR(named.st_mode)) {
            auto input = directory_at(from,name);
            if (!same_inode(named,metadata(input.value))) fail("source directory changed before opening");
            auto output = directory_at(to,name,true);
            directory(input.value,output.value,relative,depth);
            verify_entry(from,name,input.value); verify_entry(to,name,output.value);
        } else file(from,name,to,relative,named);
    }
    void run(const std::vector<fs::path>& selected) {
        for (const auto& relative : selected) {
            tick(cancelled);
            if (excluded_path(relative)) fail("protected or excluded path selected");
            if (relative.empty()) {
                if (++entries > limits.max_entries) fail("directory entry copy budget exceeded");
                directory(source,destination,{},0); continue;
            }
            auto input = duplicate(source), output = duplicate(destination);
            std::vector<Fd> source_parents,target_parents;
            std::vector<std::string> names;
            std::size_t depth = 0;
            for (const auto& part : relative.parent_path()) {
                if (++depth > limits.max_depth || ++entries > limits.max_entries)
                    fail("directory depth/entry copy budget exceeded");
                names.push_back(part.native());
                source_parents.push_back(std::move(input)); target_parents.push_back(std::move(output));
                input = directory_at(source_parents.back().value,part.native());
                output = directory_at(target_parents.back().value,part.native(),true);
            }
            node(input.value,relative.filename().native(),output.value,relative,depth+1,true);
            if (exporting) {
                sync_file(output.value);
                for (auto index = target_parents.size(); index > 0; --index) sync_file(target_parents[index-1].value);
            }
            for (std::size_t index = 0; index < names.size(); ++index) {
                verify_entry(source_parents[index].value,names[index],index+1==names.size() ? input.value : source_parents[index+1].value);
                verify_entry(target_parents[index].value,names[index],index+1==names.size() ? output.value : target_parents[index+1].value);
            }
        }
        tick(cancelled);
    }
};
// Open a selected source before any copying. No symlink in a parent can be
// skipped just because overlapping selections will later be deduplicated.
void validate_selection(int root,const fs::path& relative) {
    if (relative.empty()) return;
    auto parent = duplicate(root);
    for (const auto& part : relative.parent_path()) parent = directory_at(parent.value,part.native());
    const auto data = entry(parent.value,relative.filename().native());
    if (!S_ISDIR(data.st_mode)) regular(data);
}
void remove_entry(int parent,const std::string& name,std::size_t depth,std::size_t& remaining) {
    if (depth > 128 || remaining == 0) fail("cleanup budget exceeded; retained owned tree");
    --remaining;
    struct stat named{};
    if (fstatat(parent,name.c_str(),&named,AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return;
        posix_fail("inspect cleanup entry");
    }
    if (S_ISDIR(named.st_mode)) {
        auto directory = directory_at(parent,name);
        if (!same_inode(named,metadata(directory.value))) fail("cleanup directory changed before opening");
        DirectoryReader reader(directory.value); std::string child;
        while (reader.next(child)) remove_entry(directory.value,child,depth+1,remaining);
        verify_entry(parent,name,directory.value);
        if (unlinkat(parent,name.c_str(),AT_REMOVEDIR) != 0) posix_fail("remove owned directory");
    } else {
        // unlinkat removes a symlink/hardlink entry itself; no target mode,
        // ownership or ACL is changed, even for a read-only external target.
        if (unlinkat(parent,name.c_str(),0) != 0) posix_fail("remove owned file entry");
    }
}
struct Measurer {
    const BuildCopyLimits& limits;
    const std::atomic_bool& cancelled;
    BuildCopyStats stats;
    std::size_t entries = 0;
    void directory(int descriptor,std::size_t depth) {
        tick(cancelled);
        if (depth > limits.max_depth || ++entries > limits.max_entries) fail("directory measurement budget exceeded");
        DirectoryReader reader(descriptor); std::string name;
        while (reader.next(name)) {
            tick(cancelled);
            const auto named = entry(descriptor,name);
            if (S_ISDIR(named.st_mode)) {
                auto child = directory_at(descriptor,name);
                if (!same_inode(named,metadata(child.value))) fail("measurement directory changed");
                directory(child.value,depth+1); verify_entry(descriptor,name,child.value);
            } else {
                if (depth+1 > limits.max_depth || ++entries > limits.max_entries) fail("entry measurement budget exceeded");
                regular(named); budget(limits,stats,static_cast<std::uint64_t>(named.st_size));
                ++stats.files; stats.bytes += static_cast<std::uint64_t>(named.st_size);
            }
        }
    }
};
}

std::string read_build_manifest_snapshot(const fs::path& path,std::size_t max_bytes) {
    checked_absolute(path);
    Anchor parent(path.parent_path()); const auto name = path.filename().native(); component(name);
    const auto named = entry(parent.fd(),name); regular(named);
    Fd file(openat(parent.fd(),name.c_str(),O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (file.value < 0) posix_fail("open build manifest");
    const auto before = metadata(file.value); regular(before);
    if (!same_inode(named,before)) fail("build manifest changed before opening");
    const auto size = static_cast<std::uint64_t>(before.st_size);
    if (size > max_bytes) fail("build manifest exceeds byte limit");
    std::string result; result.reserve(static_cast<std::size_t>(size));
    std::array<char,4096> bytes{};
    for (;;) {
        const auto count = read(file.value,bytes.data(),bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) posix_fail("read build manifest");
        if (!count) break;
        if (static_cast<std::uint64_t>(count) > size-result.size()) fail("build manifest grew while reading");
        result.append(bytes.data(),static_cast<std::size_t>(count));
    }
    if (result.size() != size || !unchanged(before,metadata(file.value))) fail("build manifest changed while reading");
    verify_entry(parent.fd(),name,file.value); parent.verify();
    return result;
}

BuildCopyStats import_build_tree(const fs::path& source,const fs::path& target,const BuildImportOptions& options,
    const std::atomic_bool& cancelled) {
    checked_absolute(source); checked_absolute(target); tick(cancelled);
    if (under(source,target) || under(target,source)) fail("import source and destination must be disjoint");
    Anchor input(source),output(target);
    if (same_inode(metadata(input.fd()),metadata(output.fd()))) fail("import source and destination alias each other");
    Copier copier{input.fd(),output.fd(),options.limits,cancelled};
    copier.workspace_exclusions = options.workspace_exclusions;
    copier.protected_paths = protections(source,options.protected_paths);
    for (const auto& path : options.excludes) copier.excluded.push_back(checked_relative(path));
    for (const auto& path : options.inputs) {
        const auto relative = checked_relative(path);
        if (copier.excluded_path(relative)) fail("protected or excluded path selected");
        validate_selection(input.fd(),relative);
    }
    copier.run(selections(options.inputs));
    input.verify(); output.verify();
    return copier.stats;
}

Json export_build_artifacts(const fs::path& source,const std::vector<fs::path>& outputs,const fs::path& host,
    const fs::path& artifact_directory,const std::vector<fs::path>& protected_paths,const BuildCopyLimits& limits,
    const std::atomic_bool& cancelled) {
    checked_absolute(source); checked_absolute(host); tick(cancelled);
    if (under(source,host) || under(host,source)) fail("artifact source and host workspace must be disjoint");
    const auto relative = checked_relative(artifact_directory,false);
    if (reserved(relative) || matches(relative,protections(host,protected_paths))) fail("protected artifact destination");
    const auto selected = selections(outputs);
    Anchor input(source),output(host);
    if (same_inode(metadata(input.fd()),metadata(output.fd()))) fail("artifact source and destination alias each other");
    for (const auto& value : outputs) {
        const auto path = checked_relative(value);
        if (reserved(path)) fail("protected output selected");
        validate_selection(input.fd(),path);
    }
    for (const auto& part : relative) output.append(part.native(),true);
    const auto id = new_id(), staging_name = ".codestudio-staging-" + id;
    if (mkdirat(output.fd(),staging_name.c_str(),0700) != 0) posix_fail("create exclusive artifact staging directory");
    try {
        auto staging = directory_at(output.fd(),staging_name);
        auto files = directory_at(staging.value,"files",true);
        Copier copier{input.fd(),files.value,limits,cancelled}; copier.exporting = true;
        copier.run(selected);
        const auto published = host / relative / id;
        Json manifest{{"schema_version",2},{"files",copier.files},{"file_count",copier.stats.files},{"bytes",copier.stats.bytes},
            {"directory",path_utf8(published)},{"manifest",path_utf8(published / "manifest.json")}};
        const auto text = manifest.dump(2) + "\n";
        if (text.size() > limits.max_manifest_bytes) fail("serialized artifact manifest exceeds byte limit");
        Fd file(openat(staging.value,"manifest.json",O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600));
        if (file.value < 0) posix_fail("create artifact manifest");
        write_all(file.value,text.data(),text.size()); sync_file(file.value); sync_file(files.value); sync_file(staging.value);
        tick(cancelled); input.verify(); output.verify(); verify_entry(output.fd(),staging_name,staging.value);
        // renameat2 rejects every pre-existing destination, including an empty
        // directory, unlike ordinary rename. No copy fallback is permitted.
        if (syscall(SYS_renameat2,output.fd(),staging_name.c_str(),output.fd(),id.c_str(),RENAME_NOREPLACE) != 0)
            posix_fail("atomically publish artifact bundle");
        // Publication is the commit point. Returning the already-built receipt
        // must not turn a committed bundle into an apparent retryable failure.
        (void)fsync(output.fd());
        return manifest;
    } catch (...) {
        try { std::size_t remaining = 1000000; remove_entry(output.fd(),staging_name,0,remaining); } catch (...) {}
        throw;
    }
}

BuildCopyStats measure_build_tree(const fs::path& root,const BuildCopyLimits& limits,const std::atomic_bool& cancelled) {
    Anchor input(root); Measurer measurer{limits,cancelled};
    measurer.directory(input.fd(),0); tick(cancelled); input.verify();
    return measurer.stats;
}

void safe_remove_owned_tree(const fs::path& path) {
    checked_absolute(path);
    if (path == path.root_path()) fail("refusing cleanup of filesystem root");
    Anchor parent(path.parent_path()); component(path.filename());
    std::size_t remaining = 1000000;
    remove_entry(parent.fd(),path.filename().native(),0,remaining); parent.verify();
}
}
#endif
