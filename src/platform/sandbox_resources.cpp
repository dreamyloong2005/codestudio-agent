#include "codestudio/sandbox_resources.hpp"
#include <stdexcept>
#include <utility>

namespace codestudio {
namespace { thread_local std::string task_identity; }
SandboxTaskScope::SandboxTaskScope(std::string task_id) : previous_(std::move(task_identity)) {
    task_identity=std::move(task_id);
}
SandboxTaskScope::~SandboxTaskScope() { task_identity=std::move(previous_); }
std::string sandbox_task_id() { return task_identity; }

#if !defined(_WIN32) && !defined(__linux__)
struct SandboxResourceLease::Impl {};
std::filesystem::path sandbox_resource_registry_path() { return {}; }
SandboxResourceLease::SandboxResourceLease(const std::string&,const std::string&,
    const std::filesystem::path&,const std::string&) { throw std::runtime_error("Sandbox resources are unavailable on this platform"); }
SandboxResourceLease::~SandboxResourceLease() = default;
void SandboxResourceLease::expect(const std::string&,const std::filesystem::path&) { throw std::runtime_error("Unsupported sandbox resources"); }
void SandboxResourceLease::track(const std::string&,const std::filesystem::path&) { throw std::runtime_error("Unsupported sandbox resources"); }
void SandboxResourceLease::record_job(const std::string&) { throw std::runtime_error("Unsupported sandbox resources"); }
void SandboxResourceLease::mark_ready() { throw std::runtime_error("Unsupported sandbox resources"); }
void SandboxResourceLease::finish() { throw std::runtime_error("Unsupported sandbox resources"); }
Json list_sandbox_resources(const std::string&,unsigned) { throw std::runtime_error("Sandbox resources are unavailable on this platform"); }
Json reclaim_sandbox_resource(const std::string&,bool,const std::atomic_bool&) { throw std::runtime_error("Sandbox resources are unavailable on this platform"); }
#endif
}
