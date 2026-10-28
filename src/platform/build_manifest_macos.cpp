#ifdef __APPLE__
#include "codestudio/build_workspace.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace codestudio {
namespace {
[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(std::string("build manifest: ") + message);
}
struct Descriptor {
    int value;
    explicit Descriptor(int fd) : value(fd) { if (value < 0) fail("cannot open anchored file"); }
    ~Descriptor() { if (value >= 0) close(value); }
    Descriptor(Descriptor&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Descriptor(const Descriptor&) = delete;
};
struct stat metadata(int descriptor) {
    struct stat result{};
    if (fstat(descriptor, &result) != 0) fail("cannot inspect open file");
    return result;
}
struct stat entry(int parent, const std::string& name) {
    struct stat result{};
    if (fstatat(parent, name.c_str(), &result, AT_SYMLINK_NOFOLLOW) != 0)
        fail("cannot inspect directory entry");
    return result;
}
bool same_inode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
        (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}
void regular(const struct stat& value) {
    if (!S_ISREG(value.st_mode) || value.st_nlink != 1 || value.st_size < 0)
        fail("a regular file without links is required");
}
bool unchanged(const struct stat& left, const struct stat& right) {
    return same_inode(left, right) && left.st_size == right.st_size && left.st_nlink == right.st_nlink &&
        left.st_mode == right.st_mode && left.st_uid == right.st_uid && left.st_gid == right.st_gid &&
        left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec && left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
        left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec && left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
}
}

std::string read_build_manifest_snapshot(const std::filesystem::path& path, std::size_t max_bytes) {
    if (!path.is_absolute()) fail("an absolute path is required");
    std::vector<std::string> names;
    for (const auto& part : path.relative_path()) {
        const auto name = part.native();
        if (name.empty() || name == "." || name == ".." || name.find('\0') != std::string::npos)
            fail("invalid path component");
        names.push_back(name);
    }
    if (names.empty()) fail("a regular file is required");
    // Keep every ancestor open, refuse symlinks at each step, and revalidate
    // the complete directory chain before accepting the captured bytes.
    std::vector<Descriptor> anchors;
    anchors.emplace_back(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    for (std::size_t i = 0; i + 1 < names.size(); ++i)
        anchors.emplace_back(openat(anchors.back().value, names[i].c_str(),
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    const auto parent = anchors.back().value;
    const auto& name = names.back();
    const auto named = entry(parent, name); regular(named);
    Descriptor file(openat(parent, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    const auto before = metadata(file.value); regular(before);
    if (!same_inode(named, before)) fail("file changed before opening");
    const auto size = static_cast<std::uint64_t>(before.st_size);
    if (size > max_bytes) fail("byte limit exceeded");
    std::string result; result.reserve(static_cast<std::size_t>(size));
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = read(file.value, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail("cannot read opened file");
        if (count == 0) break;
        if (static_cast<std::uint64_t>(count) > size - result.size()) fail("file grew while reading");
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (result.size() != size || !unchanged(before, metadata(file.value)) || !same_inode(before, entry(parent, name)))
        fail("file changed while reading");
    for (std::size_t i = 0; i + 1 < names.size(); ++i)
        if (!same_inode(entry(anchors[i].value, names[i]), metadata(anchors[i + 1].value)))
            fail("ancestor changed while reading");
    return result;
}
}
#endif
