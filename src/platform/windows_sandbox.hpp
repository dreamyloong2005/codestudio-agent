#pragma once

#ifdef _WIN32
#include "codestudio/types.hpp"
#include <windows.h>
#include <memory>
#include <map>

namespace codestudio {
struct WindowsBuildLimits {
    DWORD processes = 64;
    SIZE_T memory_bytes = 4ULL * 1024 * 1024 * 1024;
    DWORD cpu_percent = 80;
};
// Owns a unique temporary AppContainer profile and an ephemeral staging tree.
// No grants are added to the original executable or workspace.
class WindowsSandbox {
public:
    explicit WindowsSandbox(const ProcessRequest& request, const WindowsBuildLimits* build = nullptr,
                            const std::atomic_bool* cancelled = nullptr);
    ~WindowsSandbox();
    WindowsSandbox(const WindowsSandbox&) = delete;
    WindowsSandbox& operator=(const WindowsSandbox&) = delete;

    static constexpr DWORD extra_attribute_count = 3;
    const std::filesystem::path& executable() const;
    const std::filesystem::path& working_directory() const;
    const std::vector<std::string>& arguments() const;
    const std::vector<wchar_t>& environment() const;
    const std::filesystem::path& readonly_root() const;
    const std::filesystem::path& writable_root() const;
    std::filesystem::path build_path(const std::filesystem::path& physical) const;
    void configure_build(const std::filesystem::path& executable,
                         const std::filesystem::path& directory,
                         const std::map<std::string, std::string>& environment);
    void apply_attributes(LPPROC_THREAD_ATTRIBUTE_LIST attributes);
    // Retains a job reference; cleanup first terminates and drains its processes.
    void supervise_job(HANDLE job);
    // JOB_LIST assigns the process atomically during CreateProcess. Call while suspended.
    void verify(HANDLE process) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
ProcessResult run_build_process(const ProcessRequest&, const std::atomic_bool&, WindowsSandbox&);
}
#endif
