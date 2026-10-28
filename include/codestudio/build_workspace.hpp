#pragma once

#include "codestudio/types.hpp"
#include <cstdint>

namespace codestudio {
struct BuildCopyLimits {
    std::size_t max_files = 100000;
    std::uint64_t max_bytes = 8ULL * 1024 * 1024 * 1024;
    std::uint64_t max_file_bytes = 2ULL * 1024 * 1024 * 1024;
    std::size_t max_entries = 200000;
    std::size_t max_depth = 64;
    std::size_t max_manifest_bytes = 1024 * 1024;
};

struct BuildImportOptions {
    std::vector<std::filesystem::path> inputs;
    std::vector<std::filesystem::path> excludes;
    // Absolute paths or paths relative to source_root. A protected ancestor
    // protects the entire root; disjoint paths are ignored. Ambiguous aliases
    // may be rejected rather than treated as disjoint.
    std::vector<std::filesystem::path> protected_paths;
    // Disable only when importing a separately approved toolchain directory.
    bool workspace_exclusions = true;
    BuildCopyLimits limits;
};

struct BuildCopyStats {
    std::size_t files = 0;
    std::uint64_t bytes = 0;
};

// Frozen recipe bytes from a single regular-file handle. Rejects links and
// anchored ancestor symlinks/reparse points; never canonicalizes away an unsafe alias.
std::string read_build_manifest_snapshot(const std::filesystem::path& path,
    std::size_t max_bytes = 1024 * 1024);

// destination must already exist and be private to the caller. Never overwrites
// files. Windows uses destination ACL inheritance; Linux/macOS normalize modes
// to 0600 or 0700, retaining only whether the source was executable. macOS clears
// inherited ACLs on newly created objects before writing any source bytes.
// Throws on cancellation, budget exhaustion, unsafe paths, or any I/O error.
BuildCopyStats import_build_tree(const std::filesystem::path& source_root,
    const std::filesystem::path& destination, const BuildImportOptions& options,
    const std::atomic_bool& cancelled);

// Caller MUST terminate/reap the isolated process tree before exporting.
// Publishes one bundle atomically: files/<output path> and manifest.json.
// No bundle is published on failure. Hard-linked outputs are rejected.
Json export_build_artifacts(const std::filesystem::path& private_workspace,
    const std::vector<std::filesystem::path>& outputs,
    const std::filesystem::path& host_workspace,
    const std::filesystem::path& artifact_directory,
    const std::vector<std::filesystem::path>& protected_paths,
    const BuildCopyLimits& limits, const std::atomic_bool& cancelled);

// Strict polling snapshot, not an OS-enforced disk quota. Throws on unsafe or
// unreadable entries and limit exhaustion. Concurrent mutation may fail a scan.
BuildCopyStats measure_build_tree(const std::filesystem::path& root,
    const BuildCopyLimits& limits, const std::atomic_bool& cancelled);

// Only pass a directory/file created and owned by this operation. Deletes a
// symlink/reparse point itself, never its target. Never weakens target permissions.
void safe_remove_owned_tree(const std::filesystem::path& owned_tree);
}
