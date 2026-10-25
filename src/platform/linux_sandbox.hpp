#pragma once
#ifdef __linux__
#include "codestudio/types.hpp"
#include <map>
#include <memory>

namespace codestudio {
struct LinuxBuildLimits {
    unsigned processes = 64;
    std::uint64_t memory_bytes = 4ULL * 1024 * 1024 * 1024;
    unsigned cpu_percent = 80;
};
// A private root, an owned delegated cgroup and a Bubblewrap namespace lifetime.
// Host paths and credentials are never inherited by the sandboxed program.
class LinuxSandbox {
public:
    explicit LinuxSandbox(const ProcessRequest&, const LinuxBuildLimits* build = nullptr,
                          const std::atomic_bool* cancelled = nullptr);
    ~LinuxSandbox();
    LinuxSandbox(const LinuxSandbox&) = delete;
    LinuxSandbox& operator=(const LinuxSandbox&) = delete;
    static void validate_available();
    const std::filesystem::path& readonly_root() const;
    const std::filesystem::path& writable_root() const;
    std::filesystem::path build_path(const std::filesystem::path&) const;
    void configure_build(const std::filesystem::path&, const std::filesystem::path&,
                         const std::map<std::string,std::string>&);
    std::vector<std::string> arguments(const ProcessRequest&) const;
    // Async-signal-safe operations only. Called in the fork child before exec.
    int child_setup() const noexcept;
    void terminate() noexcept;
    bool populated() const;
    void quiesce();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
ProcessResult run_build_process(const ProcessRequest&, const std::atomic_bool&, LinuxSandbox&);
}
#endif
