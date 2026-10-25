#pragma once
#include "codestudio/types.hpp"
#include <memory>

namespace codestudio {
// Generic, immutable file inventory for a process and its private dependencies.
// Argument templates use ${package}/relative/path; no shell or host lookup.
class ProcessPackage {
public:
    ProcessPackage(const std::filesystem::path& source, const Json& files,
                   const std::filesystem::path& entrypoint, const std::vector<std::string>& arguments);
    const std::filesystem::path& entrypoint() const;
    const std::filesystem::path& source() const;
    std::vector<std::string> arguments(const std::filesystem::path& visible_root) const;
    // Destination is an existing empty private directory. Copy exact listed
    // files, then hash and verify modes; never launch on partial preparation.
    void stage(const std::filesystem::path& destination, const std::atomic_bool& cancelled) const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
void validate_package_limits(const ProcessRequest& request);
}
