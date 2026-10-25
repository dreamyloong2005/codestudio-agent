#include "codestudio/mcp.hpp"

#include <chrono>
#include <fstream>
#include <functional>
#include <random>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
using namespace codestudio;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("mcp: ") + message);
}
void rejected(const std::function<void()>& action, const char* message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    check(threw, message);
}
std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
struct Temp {
    fs::path root;
    Temp() {
        std::random_device random;
        for (unsigned i = 0; i < 32; ++i) {
            auto candidate = fs::temp_directory_path() / ("codestudio-mcp-test-" + std::to_string(random()));
            if (fs::create_directory(candidate)) { root = std::move(candidate); return; }
        }
        throw std::runtime_error("Cannot create MCP test directory.");
    }
    ~Temp() { std::error_code error; fs::remove_all(root, error); }
};
void write(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary); output << contents;
    check(output.good(), "manifest fixture writes");
}
Json server(const fs::path& executable, const std::string& mode, const std::string& id = "fixture", int timeout = 2000) {
    return {{"id", id}, {"command", utf8(executable)}, {"args", Json::array({mode})},
            {"tools", Json::array({"echo"})}, {"timeout_ms", timeout}};
}
ToolCall call() { return {"request", "mcp_fixture_echo", {{"text", "hello"}}}; }
}

void run_mcp_tests(const std::filesystem::path& fixture_path) {
    Temp temp;
    const auto fixture = fs::canonical(fixture_path);
    const auto manifest = temp.root / "plugins.json";
    const auto marker = temp.root / "started.txt";
    const std::atomic_bool cancelled{false};
    const auto put = [&](const Json& servers) { write(manifest, Json({{"version", 2}, {"servers", servers}}).dump()); };
    const auto load = [&](const std::string& mode, int timeout = 2000) {
        put(Json::array({server(fixture, mode, "fixture", timeout)}));
        return load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled);
    };

    {
        put(Json::array({server(fixture, "normal")}));
        McpPlan plan(manifest, temp.root, {"fixture"});
        const auto snapshot = plan.snapshot();
        write(manifest, "malformed replacement");
        auto extensions = plan.connect({}, cancelled);
        check(plan.snapshot() == snapshot && extensions.front()->execute(call(), cancelled).ok,
              "frozen plan never rereads a replaced manifest during connect");
        check(plan.credential_references().empty(), "stdio has no implicit credential reference");
    }

    {
        auto local = server(fixture, "normal");
        const auto preflight_marker = temp.root / "credential-preflight.started";
        local["args"].push_back(utf8(preflight_marker));
        Json remote{{"id", "remote"}, {"transport", "streamable-http"}, {"url", "https://example.invalid/mcp"},
                    {"credential", "env:CODESTUDIO_TEST_REMOTE"}, {"tools", Json::array({"echo"})}};
        put(Json::array({local, remote}));
        McpPlan plan(manifest, temp.root, {"fixture", "remote"});
        check(plan.credential_references() == std::vector<std::string>{"env:CODESTUDIO_TEST_REMOTE"},
              "remote credentials belong to the frozen granted endpoint");
        rejected([&] { plan.connect([](const std::string&) -> std::string { throw std::runtime_error("missing"); }, cancelled); },
                 "credential resolution failure refuses entire connection plan");
        check(!fs::exists(preflight_marker), "all credentials resolved before earlier stdio server launch");
        rejected([&] { plan.connect([](const std::string&) { return std::string("invalid\r\ncredential"); }, cancelled); },
                 "invalid credential rejected before connection plan effects");
        check(!fs::exists(preflight_marker), "invalid remote credential does not launch earlier server");
    }

    auto marked = server(fixture, "normal");
    marked["args"].push_back(utf8(marker));
    for (const auto& version : {Json(0), Json(1), Json(3), Json(999), Json(2.0), Json(2.5), Json(true), Json("2"), Json(nullptr)}) {
        write(manifest, Json({{"version", version}, {"servers", Json::array({marked})}}).dump());
        rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled); },
                 "unsupported manifest version rejected before granted server launch");
        check(!fs::exists(marker), "unsupported manifest version never launches server");
        rejected([&] { load_mcp_extensions(manifest, temp.root, {}, cancelled); },
                 "ungranted servers do not bypass manifest version validation");
    }
    write(manifest, Json({{"servers", Json::array({marked})}}).dump());
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled); },
             "missing manifest version rejected before granted server launch");
    check(!fs::exists(marker), "missing manifest version never launches server");
    put(Json::array({marked}));
    check(load_mcp_extensions(manifest, temp.root, {}, cancelled).empty(), "ungranted server is not loaded");
    check(!fs::exists(marker), "ungranted server was never launched");
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"absent"}, cancelled); }, "unknown grant rejected");
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture", "fixture"}, cancelled); }, "duplicate grant rejected");
    check(!fs::exists(marker), "invalid grant must not launch server");

    auto bad = server(fixture, "normal", "second"); bad["command"] = "relative-command";
    put(Json::array({marked, bad}));
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled); }, "relative command rejected before launch");
    check(!fs::exists(marker), "all manifest validation precedes launch");
    bad = server(temp.root / "absent-executable", "normal", "second");
    put(Json::array({marked, bad}));
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture", "second"}, cancelled); }, "missing granted executable rejected");
    check(!fs::exists(marker), "missing later executable does not start earlier server");
    put(Json::array({marked, marked}));
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled); }, "duplicate server id rejected");
    auto many_a = marked;
    auto many_b = server(fixture, "normal", "second");
    many_a["tools"] = Json::array(); many_b["tools"] = Json::array();
    for (unsigned index = 0; index < 64; ++index) many_a["tools"].push_back("tool" + std::to_string(index));
    for (unsigned index = 0; index < 65; ++index) many_b["tools"].push_back("tool" + std::to_string(index));
    put(Json::array({many_a, many_b}));
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture", "second"}, cancelled); }, "aggregate grant count rejected");
    check(!fs::exists(marker), "aggregate grant count checked before any server launch");
    many_b["tools"].erase(many_b["tools"].end() - 1); // 128 MCP tools plus one sibling build tool.
    put(Json::array({many_a, many_b}));
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture", "second"}, cancelled,
        SandboxMode::none, 1, 128); }, "sibling extension reserves its tool slot");
    check(!fs::exists(marker), "combined extension count rejected before any MCP server launch");
    put(Json::array({marked}));
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled,
        SandboxMode::none, 1, extension_schema_byte_limit); }, "exhausted sibling schema budget rejected");
    check(!fs::exists(marker), "fully reserved schema budget checked before server launch");
    auto alias_a = server(fixture, "normal", "a"); alias_a["tools"] = Json::array({"b_c"});
    auto alias_b = server(fixture, "normal", "a_b"); alias_b["tools"] = Json::array({"c"});
    put(Json::array({alias_a, alias_b}));
    rejected([&] { load_mcp_extensions(manifest, temp.root, {}, cancelled); }, "ambiguous aliases rejected");
    for (const auto& malformed : {Json::array(), Json::array({"echo", "echo"}), Json::array({"dot.name"}), Json::array({std::string(64, 'a')})}) {
        bad = server(fixture, "normal"); bad["tools"] = malformed; put(Json::array({bad}));
        rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled); }, "invalid allowlist rejected");
    }
    put(Json::array({marked}));
    std::atomic_bool pre_cancelled{true};
    rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, pre_cancelled); }, "cancelled initialization rejected");
    check(!fs::exists(marker), "cancelled initialization does not launch child");
    write(manifest, "{\"version\":2,\"version\":2,\"servers\":[]}");
    rejected([&] { load_mcp_extensions(manifest, temp.root, {}, cancelled); }, "duplicate manifest JSON keys rejected");

    {
        put(Json::array({server(fixture, "normal")}));
        auto extensions = load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled, SandboxMode::none, 1, 128);
        check(extensions.size() == 1 && extensions.front()->execute(call(), cancelled).ok,
              "MCP discovery and dispatch work with sibling capacity reserved");
    }
    {
        put(Json::array({server(fixture, "normal")}));
        rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled,
            SandboxMode::none, 1, extension_schema_byte_limit - 2); }, "discovered schemas share the reserved byte budget");
    }
    {
        auto extensions = load("normal");
        check(extensions.size() == 1, "one granted server creates one extension");
        const auto schema = extensions.front()->schemas();
        check(schema.size() == 1 && schema[0]["function"]["name"] == "mcp_fixture_echo", "only exact allowlist exposed with alias");
        check(!schema[0]["function"].contains("annotations"), "server annotations do not become permission declarations");
        extensions.front()->authorize(call());
        auto result = extensions.front()->execute(call(), cancelled);
        check(result.ok && result.effect == "applied", "valid MCP call returns an execution receipt");
        check(result.output["content"][0]["text"] == "echo:hello" && result.output["structuredContent"]["calls"] == 1,
              "validated content and structured output retained");
        result = extensions.front()->execute(call(), cancelled);
        check(result.ok && result.output["structuredContent"]["calls"] == 2, "session persists across calls");
        ToolPolicy policy; policy.workspace = temp.root;
        ToolBroker broker(policy, extensions);
        result = broker.execute(call(), cancelled);
        check(result.ok && result.output["structuredContent"]["calls"] == 3, "broker dispatches extension calls");
        for (const auto& arguments : {Json::array(), Json::object(), Json({{"text", 2}}), Json({{"text", "hello"}, {"extra", true}}),
                                      Json({{"text", "hello"}, {"count", -1}}), Json({{"text", "hello"}, {"count", 11}}),
                                      Json({{"text", "hello"}, {"count", 1.5}}), Json({{"text", ""}}),
                                      Json({{"text", "hello"}, {"labels", Json::array({1})}})}) {
            auto invalid_call = call(); invalid_call.arguments = arguments;
            rejected([&] { extensions.front()->authorize(invalid_call); }, "malformed arguments denied before dispatch");
            const auto denied = extensions.front()->execute(invalid_call, cancelled);
            check(!denied.ok && denied.effect == "none", "direct execute rechecks authorization");
        }
        auto unknown = call(); unknown.name = "mcp_fixture_hidden";
        rejected([&] { extensions.front()->authorize(unknown); }, "ungranted listed tool cannot be called");
        result = extensions.front()->execute(call(), cancelled);
        check(result.output["structuredContent"]["calls"] == 4, "denied operations never reached server");
        result = extensions.front()->execute(call(), pre_cancelled);
        check(!result.ok && result.effect == "none", "pre-cancelled call never dispatched");
    }

    for (const auto& mode : {"pagination", "version-june", "version-march", "side-requests", "stderr", "content-types", "output-schema"}) {
        auto extensions = load(mode);
        const auto result = extensions.front()->execute(call(), cancelled);
        check(result.ok, "version negotiation, pagination, side requests and output handling work");
        if (std::string(mode) == "content-types") check(result.output["content"].size() == 5, "all supported MCP content types retained");
    }
    for (const auto& mode : {"version-bad", "no-tools", "malformed-list", "duplicate-json", "bad-id", "bad-cursor",
                             "page-limit", "duplicates", "duplicates-pages", "missing", "bad-schema", "tasks-required"}) {
        rejected([&] { load(mode); }, "invalid discovery fails closed");
    }
    {
        auto oversized = server(fixture, "schema-pages", "fixture", 5000);
        oversized["tools"] = Json::array();
        for (unsigned index = 0; index < 40; ++index) oversized["tools"].push_back("tool" + std::to_string(index));
        put(Json::array({oversized}));
        rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture"}, cancelled); }, "schema bytes bounded across pagination");
        oversized = server(fixture, "large-schemas", "fixture", 5000);
        oversized["tools"] = Json::array();
        for (unsigned index = 0; index < 20; ++index) oversized["tools"].push_back("tool" + std::to_string(index));
        auto second = oversized; second["id"] = "second";
        put(Json::array({oversized, second}));
        rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture", "second"}, cancelled); }, "schema bytes bounded across servers");
        oversized = server(fixture, "large-output-schema", "fixture", 5000);
        second = oversized; second["id"] = "second";
        put(Json::array({oversized, second}));
        rejected([&] { load_mcp_extensions(manifest, temp.root, {"fixture", "second"}, cancelled); }, "retained output schemas share aggregate budget");
    }
    {
        auto extensions = load("list-change-before-call");
        const auto result = extensions.front()->execute(call(), cancelled);
        check(!result.ok && result.effect == "none", "queued schema-change notification stops before tool dispatch");
        rejected([&] { extensions.front()->authorize(call()); }, "schema changes invalidate frozen session");
    }
    {
        auto extensions = load("tool-error");
        const auto result = extensions.front()->execute(call(), cancelled);
        check(!result.ok && result.effect == "unknown" && result.output["isError"] == true,
              "server tool error never claims no effects");
        check(result.output["content"][0]["text"] == "echo:hello", "valid tool error content preserved");
    }
    for (const auto& mode : {"rpc-error", "malformed-call", "bad-result", "bad-content", "bad-structured", "bad-error-flag",
                             "list-change", "flood", "eof", "bad-output-schema"}) {
        auto extensions = load(mode);
        const auto result = extensions.front()->execute(call(), cancelled);
        check(!result.ok && result.effect == "unknown", "failure after dispatch leaves effects unknown");
        check(result.error.find("sensitive") == std::string::npos, "protocol errors never echo server details");
        rejected([&] { extensions.front()->authorize(call()); }, "broken session is not reused");
    }
    {
        auto extensions = load("timeout", 200);
        const auto start = std::chrono::steady_clock::now();
        const auto result = extensions.front()->execute(call(), cancelled);
        check(!result.ok && result.effect == "unknown" && std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
              "hung MCP call has bounded deadline and unknown effect");
    }
    {
        auto extensions = load("timeout");
        std::atomic_bool cancel{false};
        std::jthread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancel = true; });
        const auto start = std::chrono::steady_clock::now();
        const auto result = extensions.front()->execute(call(), cancel);
        check(!result.ok && result.effect == "unknown" && std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
              "cancellation stops pending RPC without retrying it");
    }
}
