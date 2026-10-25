#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using Json = nlohmann::json;
namespace {
void send(const Json& message) { std::cout << message.dump() << '\n' << std::flush; }
void reply(const Json& id, const Json& result) { send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}); }
Json definition(const std::string& name) {
    return {{"name", name}, {"description", "Echo fixture text"}, {"inputSchema", {
        {"type", "object"}, {"properties", {
            {"text", {{"type", "string"}, {"minLength", 1}, {"maxLength", 32}}},
            {"count", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10}}},
            {"labels", {{"type", "array"}, {"maxItems", 2}, {"items", {{"type", "string"}}}}}}},
        {"required", Json::array({"text"})}, {"additionalProperties", false}}},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false}}}};
}
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "normal";
    if (argc > 2) {
        const std::string argument = argv[2];
        std::ofstream marker(std::filesystem::path(std::u8string(argument.begin(), argument.end())));
        marker << "started";
    }
    bool initialized = false;
    unsigned calls = 0;
    bool request_checks_ok = true;
    try {
        std::string line;
        while (std::getline(std::cin, line)) {
            const auto request = Json::parse(line);
            const auto method = request.value("method", "");
            if (method.empty()) continue; // Replies to fixture-initiated requests.
            if (method == "notifications/initialized") { initialized = true; continue; }
            const auto id = request.at("id");
            if (method == "initialize") {
                if (mode == "hang-initialize") { std::this_thread::sleep_for(std::chrono::seconds(30)); continue; }
                request_checks_ok = request.at("params").at("protocolVersion") == "2025-11-25" &&
                    request.at("params").at("capabilities").empty();
                auto version = "2025-11-25";
                if (mode == "version-june") version = "2025-06-18";
                if (mode == "version-march") version = "2025-03-26";
                if (mode == "version-bad") version = "1900-01-01";
                Json capabilities = {{"tools", {{"listChanged", true}}}};
                if (mode == "no-tools") capabilities = Json::object();
                reply(id, {{"protocolVersion", version}, {"capabilities", capabilities},
                           {"serverInfo", {{"name", "mcp-fixture"}, {"version", "1"}}}});
            } else if (method == "ping") {
                reply(id, Json::object());
            } else if (method == "tools/list") {
                if (!initialized || !request_checks_ok) return 4;
                const auto cursor = request.at("params").value("cursor", "");
                if (mode == "malformed-list") { std::cout << "not-json\n" << std::flush; continue; }
                if (mode == "duplicate-json") {
                    std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << id.dump()
                              << ",\"result\":{},\"result\":{}}\n" << std::flush;
                    continue;
                }
                if (mode == "bad-id") { reply("wrong-id", {{"tools", Json::array({definition("echo")})}}); continue; }
                if (mode == "schema-pages" || mode == "large-schemas") {
                    Json list = Json::array();
                    const unsigned start = cursor.empty() ? 0 : 20;
                    for (unsigned index = start; index < start + 20; ++index) {
                        auto tool = definition("tool" + std::to_string(index));
                        tool["description"] = std::string(30000, 'x');
                        list.push_back(std::move(tool));
                    }
                    Json result{{"tools", list}};
                    if (mode == "schema-pages" && cursor.empty()) result["nextCursor"] = "next";
                    reply(id, result);
                } else if (mode == "pagination") {
                    if (cursor.empty()) reply(id, {{"tools", Json::array({definition("hidden")})}, {"nextCursor", "next"}});
                    else reply(id, {{"tools", Json::array({definition("echo")})}});
                } else if (mode == "bad-cursor") {
                    reply(id, {{"tools", Json::array()}, {"nextCursor", "same"}});
                } else if (mode == "page-limit") {
                    reply(id, {{"tools", Json::array()}, {"nextCursor", "x" + cursor}});
                } else if (mode == "duplicates") {
                    reply(id, {{"tools", Json::array({definition("echo"), definition("echo")})}});
                } else if (mode == "duplicates-pages") {
                    if (cursor.empty()) reply(id, {{"tools", Json::array({definition("echo")})}, {"nextCursor", "next"}});
                    else reply(id, {{"tools", Json::array({definition("echo")})}});
                } else if (mode == "missing") {
                    reply(id, {{"tools", Json::array({definition("hidden")})}});
                } else if (mode == "bad-schema") {
                    auto tool = definition("echo"); tool["inputSchema"]["required"] = "text";
                    reply(id, {{"tools", Json::array({tool})}});
                } else if (mode == "tasks-required") {
                    auto tool = definition("echo"); tool["execution"] = {{"taskSupport", "required"}};
                    reply(id, {{"tools", Json::array({tool})}});
                } else if (mode == "output-schema" || mode == "bad-output-schema" || mode == "large-output-schema") {
                    auto tool = definition("echo");
                    tool["outputSchema"] = {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}},
                                            {"required", Json::array({mode == "output-schema" ? "text" : "missing"})}};
                    if (mode == "large-output-schema") tool["outputSchema"]["description"] = std::string(600000, 'x');
                    reply(id, {{"tools", Json::array({tool})}});
                } else {
                    reply(id, {{"tools", Json::array({definition("echo"), definition("hidden")})}});
                    if (mode == "list-change-before-call") send({{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
                }
            } else if (method == "tools/call") {
                ++calls;
                if (mode == "timeout") { std::this_thread::sleep_for(std::chrono::seconds(10)); continue; }
                if (mode == "eof") return 0;
                if (mode == "stderr") { for (unsigned i = 0; i < 20000; ++i) std::cerr << "ignored diagnostics\n"; std::cerr.flush(); }
                if (mode == "malformed-call") { std::cout << "sensitive-payload-not-json\n" << std::flush; continue; }
                if (mode == "rpc-error") {
                    send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32603}, {"message", "sensitive backend error"}}}});
                    continue;
                }
                if (mode == "list-change") { send({{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}}); continue; }
                if (mode == "flood") {
                    for (unsigned i = 0; i < 130; ++i) send({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", Json::object()}});
                    continue;
                }
                if (mode == "side-requests") {
                    for (const auto& name : {"sampling/createMessage", "elicitation/create", "ping"}) {
                        send({{"jsonrpc", "2.0"}, {"id", name}, {"method", name}, {"params", Json::object()}});
                        std::string response_line;
                        if (!std::getline(std::cin, response_line)) return 5;
                        const auto response = Json::parse(response_line);
                        if (response.at("id") != name) return 6;
                        if (std::string(name) == "ping") { if (response.at("result") != Json::object()) return 7; }
                        else if (response.at("error").at("code") != -32601) return 8;
                    }
                }
                if (mode == "bad-result") { reply(id, {{"content", "not-array"}}); continue; }
                if (mode == "bad-content") { reply(id, {{"content", Json::array({{{"type", "text"}, {"text", 42}}})}}); continue; }
                if (mode == "bad-structured") { reply(id, {{"content", Json::array()}, {"structuredContent", Json::array()}}); continue; }
                if (mode == "bad-error-flag") { reply(id, {{"content", Json::array()}, {"isError", "false"}}); continue; }
                const auto text = request.at("params").at("arguments").at("text").get<std::string>();
                Json content = Json::array({{{"type", "text"}, {"text", "echo:" + text}}});
                if (mode == "content-types") {
                    content.push_back({{"type", "image"}, {"data", "YQ=="}, {"mimeType", "image/png"}});
                    content.push_back({{"type", "audio"}, {"data", "YQ=="}, {"mimeType", "audio/wav"}});
                    content.push_back({{"type", "resource_link"}, {"name", "fixture"}, {"uri", "test://fixture"}});
                    content.push_back({{"type", "resource"}, {"resource", {{"uri", "test://embedded"}, {"text", "text"}}}});
                }
                reply(id, {{"content", content}, {"structuredContent", {{"text", text}, {"calls", calls}}},
                           {"isError", mode == "tool-error"}});
            } else return 9;
        }
    } catch (...) { return 3; }
    return 0;
}
