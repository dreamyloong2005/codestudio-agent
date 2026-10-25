#pragma once
#include "codestudio/types.hpp"
#include <memory>

namespace codestudio {
// Managed plugins are immutable MCP process packages. None of these operations
// execute package code or grant MCP tools. Formats are current-only format 2.
Json pack_plugin(const std::filesystem::path& source, const std::filesystem::path& output);
Json install_plugin(const std::filesystem::path& bundle, const std::string& expected_sha256,
                    const std::filesystem::path& store);
Json install_plugin_url(const std::string& url, const std::string& expected_sha256,
                        const std::filesystem::path& store, const std::atomic_bool& cancelled,
                        bool allow_http_loopback = false);
Json lock_plugin(const std::filesystem::path& installed, const std::filesystem::path& output);
Json inspect_plugin_lock(const std::filesystem::path& lock);

// A lock and its installation are checked before producing this frozen plan.
// verify() checks the SAME installed manifest/bundle/payload before launch; it
// never rereads the lock to choose a different version during the operation.
class PluginPlan {
public:
    explicit PluginPlan(const std::filesystem::path& lock);
    const std::string& snapshot() const;
    const std::string& id() const;
    const std::string& version() const;
    const std::string& bundle_sha256() const;
    const std::filesystem::path& directory() const;
    const std::filesystem::path& executable() const;
    const std::vector<std::string>& arguments() const;
    const std::vector<std::string>& tools() const;
    std::size_t retained_bytes() const;
    std::size_t payload_bytes() const;
    std::shared_ptr<const ProcessPackage> process_package() const;
    void verify() const;
private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
}
