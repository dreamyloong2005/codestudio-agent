#pragma once
#include "codestudio/types.hpp"
#include <memory>

namespace codestudio {
// Resolves the current user's control directory without creating it. Host file
// tools and workspace imports must protect this directory from task access.
std::filesystem::path sandbox_resource_registry_path();
// Internal synchronous task correlation, restored on scope exit. It conveys no
// authority and is not inherited by threads or child processes.
class SandboxTaskScope {
public:
    explicit SandboxTaskScope(std::string task_id);
    ~SandboxTaskScope();
    SandboxTaskScope(const SandboxTaskScope&) = delete;
    SandboxTaskScope& operator=(const SandboxTaskScope&) = delete;
private:
    std::string previous_;
};
std::string sandbox_task_id();

// Private host-side ownership journal. Never grants access to a task or permits
// replay of an action. Only the OS sandbox adapter creates these leases.
class SandboxResourceLease {
public:
    // id is 32 lowercase hex; kind is process, package, or build. An empty
    // task_id denotes discovery/pre-task work, never an invented task identity.
    SandboxResourceLease(const std::string& id, const std::string& kind,
                         const std::filesystem::path& workspace, const std::string& task_id);
    ~SandboxResourceLease();
    SandboxResourceLease(const SandboxResourceLease&) = delete;
    SandboxResourceLease& operator=(const SandboxResourceLease&) = delete;
    // Persist intended path before exclusive resource creation, then capture its
    // native directory identity immediately afterwards. An uncaptured resource
    // that exists is ambiguous and must never be adopted by a reclaimer.
    void expect(const std::string& kind, const std::filesystem::path& path);
    void track(const std::string& kind, const std::filesystem::path& path);
    // Windows only: persist each owned Job name before exclusive creation.
    void record_job(const std::string& name);
    void mark_ready();
    // Remove the journal only after every resource is proved absent. Best
    // effort callers may catch failure; the destructor only releases the lock.
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// No registry creation, process signalling, database writes, or resource
// mutation. Bounded lexicographic pages; after is empty or a 32-hex resource id.
// {format:2,registry,items:[{id,kind,workspace,task_id,owner,phase,state,reason,
// resources,...}],next_after:null|string}. State: active/reclaimable/blocked/invalid.
Json list_sandbox_resources(const std::string& after = {}, unsigned limit = 50);
// Single exact registered id; dry-run does not create or mutate any file.
// State: reclaimable (dry-run), reclaimed, absent, active, blocked, cancelled,
// or incomplete. Return {format:2,id,dry_run,state,reason,resources,...}.
// Never signal a PID or kill a cgroup/Job. Lock+birth identity+empty process
// group+resource identities must all agree; unsafe or unknown state is refusal.
Json reclaim_sandbox_resource(const std::string& id, bool dry_run,
                              const std::atomic_bool& cancelled);
}
