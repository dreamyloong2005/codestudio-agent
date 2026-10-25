#include "codestudio/build_proposal.hpp"
#include "codestudio/digest.hpp"
#include "codestudio/store.hpp"
#include <algorithm>
#include <stdexcept>

namespace {
using namespace codestudio;
namespace fs = std::filesystem;
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("build proposal: " + message);
}
template<class F> void rejects(F&& action, const char* message) {
    bool denied = false;
    try { action(); } catch (const std::exception&) { denied = true; }
    check(denied, message);
}
struct Temp {
    fs::path root = fs::temp_directory_path() / ("codestudio-proposal-tests-" + new_id());
    Temp() { fs::create_directory(root); root = fs::canonical(root); }
    ~Temp() { std::error_code error; fs::remove(root, error); }
};
Json arguments(const fs::path& workspace) {
    return {{"reason","Compile and run the requested tests"}, {"manifest",{
        {"version",2},{"inputs",Json::array({"missing-source.cpp"})},
        {"mounts",Json::array({{{"name","compiler"},{"source",path_utf8(workspace / "missing-toolchain")},
                               {"inputs",Json::array({"tool.exe"})}}})},
        {"steps",Json::array({{{"executable","${compiler}/tool.exe"},
                              {"arguments",Json::array({"missing-source.cpp"})}}})},
        {"outputs",Json::array({"out/program.exe"})},{"artifact_directory","artifacts"}}}};
}
}

void run_build_proposal_tests() {
    using namespace codestudio;
#if defined(_WIN32) || defined(__linux__)
    check(sha256_bytes("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA256 vector");
    check(sha256_bytes("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc SHA256 vector");
    check(sha256_bytes(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "incremental SHA256 vector");
    Temp temp;
    const auto args = arguments(temp.root);
    const std::vector<fs::path> protected_paths{temp.root / ".codestudio", temp.root / "build.json"};
    const auto proposal = inspect_build_proposal(args, temp.root, protected_paths);
    check(proposal.at("status") == "pending_approval", "proposal implied execution approval");
    check(proposal.at("version") == 2 && proposal.at("manifest") == args.at("manifest") &&
          proposal.at("recipe") == args.at("manifest"),
          "proposal lost original or expanded configuration");
    check(proposal.at("protected_paths").size() == 2, "host exclusions missing from review data");
    check(fs::is_empty(temp.root), "proposal inspected or populated missing inputs/toolchain");
    check(validate_build_proposal_receipt(args, proposal, temp.root, protected_paths) == proposal,
          "unchanged proposal cannot be revalidated");

    // The digest binds what is reviewed, including the normalized recipe and
    // host-generated exclusions. It must not depend on JSON insertion order.
    auto reordered = Json::parse(args.dump());
    check(inspect_build_proposal(reordered, temp.root, protected_paths).at("digest") == proposal.at("digest"),
          "equivalent configuration changed digest");
    for (const auto* field : {"version", "reason", "recipe", "workspace", "protected_paths", "digest"}) {
        auto changed = proposal;
        changed[field] = "tampered";
        rejects([&] { (void)validate_build_proposal_receipt(args, changed, temp.root, protected_paths); },
                "modified proposal receipt was accepted");
    }
    auto changed_args = args;
    changed_args["manifest"]["outputs"] = Json::array({"other.exe"});
    rejects([&] { (void)validate_build_proposal_receipt(changed_args, proposal, temp.root, protected_paths); },
            "changed model proposal reused review receipt");
    auto fewer_exclusions = protected_paths;
    fewer_exclusions.pop_back();
    rejects([&] { (void)validate_build_proposal_receipt(args, proposal, temp.root, fewer_exclusions); },
            "host exclusion change did not invalidate receipt");

    const auto deny = [&](const Json& invalid) {
        rejects([&] { (void)inspect_build_proposal(invalid, temp.root, protected_paths); }, "unsafe proposal accepted");
        check(fs::is_empty(temp.root), "rejected proposal caused filesystem effects");
    };
    auto invalid = args; invalid["approved"] = true; deny(invalid);
    invalid = args; invalid["manifest"]["unexpected"] = Json::object(); deny(invalid);
    invalid = args; invalid["protected_paths"] = Json::array(); deny(invalid);
    invalid = args; invalid["reason"] = ""; deny(invalid);
    invalid = args; invalid["manifest"]["version"] = 1; deny(invalid);
    invalid = args; invalid["manifest"]["version"] = true; deny(invalid);
    invalid = args; invalid["manifest"]["artifact_directory"] = "../outside"; deny(invalid);
    invalid = args; invalid["manifest"]["environment"] = {{"HOME","${workspace}"}}; deny(invalid);
    invalid = args; invalid["manifest"]["steps"][0]["arguments"] = Json::array({std::string(65536, 'x')}); deny(invalid);

    BuildProposal tool(temp.root, protected_paths);
    const ToolCall call{"proposal-call","propose_build",args};
    std::atomic_bool cancelled{false};
    tool.authorize(call);
    const auto result = tool.execute(call, cancelled);
    check(result.ok && result.effect == "none" && result.output == proposal,
          "proposal tool reported effects or changed validation data");
    cancelled = true;
    const auto stopped = tool.execute(call, cancelled);
    check(!stopped.ok && stopped.effect == "none" && stopped.output.at("cancelled") == true,
          "cancelled proposal caused execution");
    cancelled = false;
    auto wrong = call; wrong.name = "build_workspace";
    const auto rejected = tool.execute(wrong, cancelled);
    check(!rejected.ok && rejected.effect == "none", "proposal tool accepted an execution call");

    // Inspect strings rather than serialized substrings: quoting/newlines in a
    // configured credential must not survive by escaping in JSON output.
    const std::string secret = "quoted\"credential\nvalue";
    BuildProposal private_tool(temp.root, protected_paths, {secret});
    auto sensitive = call; sensitive.arguments["reason"] = secret;
    rejects([&] { private_tool.authorize(sensitive); }, "escaped credential reached persistence");
    const auto hidden = private_tool.execute(sensitive, cancelled);
    check(!hidden.ok && hidden.effect == "none" && hidden.error.find(secret) == std::string::npos,
          "rejected credential was exposed in an error");
    sensitive = call; sensitive.arguments["manifest"]["environment"] = {{secret,"value"}};
    rejects([&] { private_tool.authorize(sensitive); }, "credential in an object key reached persistence");
    check(fs::is_empty(temp.root), "proposal workflow created source or artifact files");
#else
    rejects([] { (void)sha256_bytes("abc"); }, "unsupported digest backend silently succeeded");
#endif
}
