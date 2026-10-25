#pragma once
#include "codestudio/tools.hpp"

namespace codestudio {
inline constexpr std::size_t build_proposal_argument_limit = 64 * 1024;
inline constexpr std::size_t build_proposal_output_limit = 128 * 1024;
inline constexpr std::size_t build_proposal_protected_path_limit = 512;

// Validate and normalize a v2 manifest without importing inputs or starting tools.
// The digest identifies configuration and host-protected paths, not source or
// compiler bytes. protected_paths metadata is supplied by the host only.
Json inspect_build_proposal(const Json& arguments,
                           const std::filesystem::path& workspace,
                           const std::vector<std::filesystem::path>& protected_paths);

// Rebuild the proposal using current manifest normalization and policy, and require exact
// equality with its persisted output before it may be considered for approval.
Json validate_build_proposal_receipt(const Json& arguments, const Json& output,
                                    const std::filesystem::path& workspace,
                                    const std::vector<std::filesystem::path>& protected_paths);

// Exposes only propose_build. A successful receipt means a plan was validated;
// it neither approves the plan nor executes any build step.
class BuildProposal final : public ToolExtension {
public:
    BuildProposal(const std::filesystem::path& workspace,
                  const std::vector<std::filesystem::path>& protected_paths,
                  std::vector<std::string> secrets = {});
    Json schemas() const override;
    void authorize(const ToolCall&) const override;
    ToolResult execute(const ToolCall&, const std::atomic_bool&) const override;
private:
    Json inspect(const ToolCall&) const;
    std::filesystem::path workspace_;
    std::vector<std::filesystem::path> protected_paths_;
    std::vector<std::string> secrets_;
};
}
