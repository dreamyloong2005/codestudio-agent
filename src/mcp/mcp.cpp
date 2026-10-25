#include "codestudio/mcp.hpp"
#include "codestudio/credentials.hpp"
#include "codestudio/plugins.hpp"
#include "codestudio/process_package.hpp"
#include "transport.hpp"
#include "../provider/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

namespace codestudio {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t max_document_bytes = 1024 * 1024;
constexpr std::size_t max_tools = 512;
constexpr std::size_t max_pages = 16;
constexpr std::size_t max_side_messages = 128;

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }
void require(bool condition, const char* message) { if (!condition) invalid(message); }

bool safe_name(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
bool clean_string(const Json& value, std::size_t maximum) {
    return value.is_string() && value.get_ref<const std::string&>().size() <= maximum &&
        value.get_ref<const std::string&>().find('\0') == std::string::npos;
}
void exact_keys(const Json& object, std::initializer_list<std::string_view> names) {
    require(object.is_object(), "Invalid MCP manifest object.");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        require(std::find(names.begin(), names.end(), key) != names.end(), "Unknown MCP manifest field.");
    }
}

struct Configuration {
    std::string id;
    bool http = false;
    std::string url;
    std::string credential;
    std::optional<McpOAuthConfig> oauth;
    std::shared_ptr<PluginPlan> plugin;
    fs::path plugin_lock;
    ProcessPackageLimits package_limits;
    bool allow_http_loopback = false;
    fs::path executable;
    std::vector<std::string> arguments;
    std::vector<std::string> tools;
    std::chrono::milliseconds timeout{10000};
};

std::string read_manifest(const fs::path& manifest) {
    std::ifstream input(manifest, std::ios::binary);
    require(input.good(), "Cannot open MCP manifest.");
    std::string text;
    char chunk[4096];
    while (input.read(chunk, sizeof chunk) || input.gcount()) {
        require(text.size() + static_cast<std::size_t>(input.gcount()) <= max_document_bytes,
                "MCP manifest exceeds size limit.");
        text.append(chunk, static_cast<std::size_t>(input.gcount()));
    }
    require(!input.bad(), "Cannot read MCP manifest.");
    return text;
}

std::vector<Configuration> configurations(const std::string& text, const std::vector<std::string>& grants,
                                        std::size_t tool_limit, SandboxMode sandbox) {
    Json document;
    try { document = protocol::strict_json(text); }
    catch (...) { invalid("Invalid MCP manifest JSON."); }
    exact_keys(document, {"version", "servers"});
    require(document.contains("version") && document["version"].is_number_integer() &&
            document["version"] == 2, "Unsupported MCP manifest version.");
    require(document.contains("servers") && document["servers"].is_array() && document["servers"].size() <= 32,
            "Invalid MCP manifest server list.");
    std::set<std::string> ids, aliases, granted;
    std::size_t granted_tool_count = 0;
    for (const auto& id : grants) {
        require(safe_name(id) && id.size() <= 16 && granted.insert(id).second, "Invalid or repeated MCP server grant.");
    }
    std::vector<Configuration> result;
    for (const auto& server : document["servers"]) {
        exact_keys(server, {"id", "transport", "command", "args", "url", "credential", "oauth", "plugin_lock", "sandbox_limits", "allow_http_loopback", "tools", "timeout_ms"});
        require(server.contains("id") && clean_string(server["id"], 16), "Invalid MCP server id.");
        Configuration config;
        config.id = server["id"].get<std::string>();
        require(safe_name(config.id) && ids.insert(config.id).second, "Invalid or repeated MCP server id.");
        const auto transport = server.value("transport", Json("stdio"));
        require(transport == "stdio" || transport == "streamable-http", "Unsupported MCP transport.");
        config.http = transport == "streamable-http";
        if (server.contains("sandbox_limits")) {
            require(!config.http && server.contains("plugin_lock"),"Sandbox limits require a locked process plugin.");
            require(!granted.contains(config.id) || sandbox!=SandboxMode::none,"Plugin sandbox limits require an explicit OS sandbox grant.");
            const auto& limits=server["sandbox_limits"];
            exact_keys(limits,{"processes","memory_mb","cpu_percent"});
            const auto number=[&](const char* key,unsigned fallback,unsigned maximum,unsigned minimum=1) {
                if (!limits.contains(key)) return fallback;
                require(limits[key].is_number_integer() && limits[key]>=minimum && limits[key]<=maximum,"Invalid plugin sandbox limit.");
                return limits[key].get<unsigned>();
            };
            config.package_limits.processes=number("processes",1,32);
            config.package_limits.memory_bytes=static_cast<std::uint64_t>(number("memory_mb",512,4096,64))*1024*1024;
            config.package_limits.cpu_percent=number("cpu_percent",80,100);
        }
        if (config.http) {
            require(!server.contains("command") && !server.contains("args") && !server.contains("plugin_lock"), "HTTP MCP cannot contain process fields.");
            require(server.contains("url") && clean_string(server["url"], 8192), "Invalid MCP endpoint.");
            config.url = server["url"].get<std::string>();
            if (server.contains("allow_http_loopback")) {
                require(server["allow_http_loopback"].is_boolean(), "Invalid MCP loopback option.");
                config.allow_http_loopback = server["allow_http_loopback"].get<bool>();
            }
            mcp_transport::validate_http_endpoint(config.url, config.allow_http_loopback);
            if (server.contains("credential")) {
                require(!server.contains("oauth"), "MCP OAuth and Bearer credentials are mutually exclusive.");
                require(clean_string(server["credential"], 256), "Invalid MCP credential reference.");
                config.credential = server["credential"].get<std::string>();
                (void)parse_credential_reference(config.credential);
            }
            if (server.contains("oauth")) {
                config.oauth = mcp_oauth_configuration(server);
                config.credential = mcp_oauth_token_key(*config.oauth);
            }
            require(!granted.contains(config.id) || sandbox == SandboxMode::none,
                    "Remote MCP cannot satisfy a local process sandbox grant.");
        } else {
            require(!server.contains("url") && !server.contains("credential") && !server.contains("oauth") && !server.contains("allow_http_loopback"),
                    "Stdio MCP cannot contain HTTP fields.");
            if (server.contains("plugin_lock")) {
                require(!server.contains("command") && !server.contains("args") && clean_string(server["plugin_lock"],32768),
                        "Locked MCP plugins cannot contain command or args.");
                const auto lock = server["plugin_lock"].get<std::string>();
                config.plugin_lock = fs::path(std::u8string(lock.begin(),lock.end()));
                require(config.plugin_lock.is_absolute(), "Plugin lock must be an absolute path.");
                if (granted.contains(config.id)) {
                    config.plugin = std::make_shared<PluginPlan>(config.plugin_lock);
                    config.executable = config.plugin->executable();
                    config.arguments = config.plugin->arguments();
                }
            } else {
            require(server.contains("command") && clean_string(server["command"], 32768) &&
                    !server["command"].get_ref<const std::string&>().empty(), "Invalid MCP executable path.");
            const auto& command = server["command"].get_ref<const std::string&>();
            config.executable = fs::path(std::u8string(command.begin(), command.end()));
            require(config.executable.is_absolute(), "MCP executable must be an absolute path.");
            require(server.contains("args") && server["args"].is_array() && server["args"].size() <= 128,
                    "Invalid MCP argument list.");
            std::size_t argument_bytes = 0;
            for (const auto& argument : server["args"]) {
                require(clean_string(argument, 32768), "Invalid MCP process argument.");
                config.arguments.push_back(argument.get<std::string>());
                argument_bytes += config.arguments.back().size();
                require(argument_bytes <= 65536, "MCP process arguments exceed size limit.");
            }
            }
        }
        require(server.contains("tools") && server["tools"].is_array() && !server["tools"].empty() &&
                server["tools"].size() <= 128, "MCP servers need a nonempty exact tool allowlist.");
        std::set<std::string> names;
        for (const auto& tool : server["tools"]) {
            require(clean_string(tool, 64), "Invalid MCP tool name.");
            const auto name = tool.get<std::string>();
            const auto alias = "mcp_" + config.id + "_" + name;
            require(safe_name(name) && alias.size() <= 64 && names.insert(name).second,
                    "MCP tool names must use ASCII letters, digits, underscore or hyphen and fit the 64-character alias limit.");
            require(aliases.insert(alias).second, "MCP tool aliases collide.");
            require(aliases.size() <= max_tools, "MCP manifest exposes too many tools.");
            config.tools.push_back(name);
            if (config.plugin) {
                const auto tools = config.plugin->tools();
                require(std::find(tools.begin(),tools.end(),name)!=tools.end(), "MCP tool is absent from the locked plugin allowlist.");
            }
        }
        if (server.contains("timeout_ms")) {
            require(server["timeout_ms"].is_number_integer() && server["timeout_ms"] >= 100 &&
                    server["timeout_ms"] <= 300000, "MCP timeout must be between 100 and 300000 milliseconds.");
            config.timeout = std::chrono::milliseconds(server["timeout_ms"].get<int>());
        }
        if (granted.contains(config.id)) {
            granted_tool_count += config.tools.size();
            require(granted_tool_count <= tool_limit, "Too many granted MCP tools for the remaining extension budget.");
            if (!config.http) {
                std::error_code error;
                require(fs::is_regular_file(config.executable, error) && !error, "MCP executable is not a regular file.");
                // A locked package must launch the exact path its plan
                // verifies, never a separately resolved temporary link target.
                if (!config.plugin) config.executable = fs::canonical(config.executable);
            }
            result.push_back(std::move(config));
        }
    }
    for (const auto& id : granted) require(ids.contains(id), "MCP grant names a server absent from the manifest.");
    return result;
}

struct SchemaBudget {
    std::size_t count = 0;
    std::size_t bytes = 2; // JSON array brackets, including an empty list.
    void charge(const Json& schema, const Json& output_schema) {
        // Bound metadata retained only for result validation as well as schemas
        // exposed to the model. Output schemas must not bypass the memory cap.
        const auto addition = schema.dump().size() + (count ? 1 : 0) +
            (output_schema.is_null() ? 0 : output_schema.dump().size());
        require(count < extension_tool_limit && addition <= extension_schema_byte_limit - bytes,
                "MCP extension schemas exceed the aggregate limit.");
        bytes += addition;
        ++count;
    }
};

// This is deliberately a bounded validation subset, not a complete JSON Schema
// evaluator. Unknown keywords are forwarded; the server still must validate its
// full schema. None of these checks grant capabilities or trust annotations.
void check_schema(const Json& schema, unsigned depth = 0) {
    require(depth <= 32 && (schema.is_object() || schema.is_boolean()), "Invalid MCP input schema.");
    if (schema.is_boolean()) return;
    const auto known_type = [](const Json& type) {
        return type.is_string() && (type == "object" || type == "array" || type == "string" ||
            type == "number" || type == "integer" || type == "boolean" || type == "null");
    };
    if (schema.contains("type")) {
        const auto& type = schema["type"];
        if (type.is_array()) {
            require(!type.empty() && type.size() <= 7, "Invalid MCP schema type.");
            for (const auto& item : type) require(known_type(item), "Invalid MCP schema type.");
        } else require(known_type(type), "Invalid MCP schema type.");
    }
    if (schema.contains("properties")) {
        require(schema["properties"].is_object() && schema["properties"].size() <= 512, "Invalid MCP schema properties.");
        for (const auto& property : schema["properties"]) check_schema(property, depth + 1);
    }
    if (schema.contains("required")) {
        require(schema["required"].is_array() && schema["required"].size() <= 512, "Invalid MCP schema required fields.");
        std::set<std::string> fields;
        for (const auto& field : schema["required"])
            require(clean_string(field, 4096) && fields.insert(field.get<std::string>()).second,
                    "Invalid MCP schema required field.");
    }
    for (const auto* key : {"additionalProperties", "items"}) {
        if (schema.contains(key)) check_schema(schema[key], depth + 1);
    }
    if (schema.contains("enum")) require(schema["enum"].is_array() && !schema["enum"].empty(), "Invalid MCP schema enum.");
    for (const auto* key : {"minItems", "maxItems", "minLength", "maxLength", "minProperties", "maxProperties"})
        if (schema.contains(key)) require(schema[key].is_number_integer() && schema[key] >= 0,
                                          "Invalid MCP schema bound.");
    for (const auto* key : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"})
        if (schema.contains(key)) require(schema[key].is_number(), "Invalid MCP schema numeric bound.");
}

bool matches_type(const Json& value, const Json& type) {
    if (type.is_array()) return std::any_of(type.begin(), type.end(), [&](const Json& t) { return matches_type(value, t); });
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer() ||
        (value.is_number_float() && std::floor(value.get<double>()) == value.get<double>());
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    return type == "null" && value.is_null();
}
void validate_arguments(const Json& value, const Json& schema, unsigned depth = 0) {
    require(depth <= 32, "MCP arguments exceed nesting limit.");
    if (schema.is_boolean()) { require(schema == true, "MCP arguments are disallowed by schema."); return; }
    if (schema.contains("type")) require(matches_type(value, schema["type"]), "MCP argument type does not match schema.");
    if (schema.contains("enum")) require(std::find(schema["enum"].begin(), schema["enum"].end(), value) != schema["enum"].end(),
                                         "MCP argument is absent from schema enum.");
    if (schema.contains("const")) require(value == schema["const"], "MCP argument does not match schema constant.");
    const auto size_bounds = [&](std::size_t count, const char* minimum, const char* maximum) {
        if (schema.contains(minimum)) require(Json(count) >= schema[minimum], "MCP arguments are below schema size bound.");
        if (schema.contains(maximum)) require(Json(count) <= schema[maximum], "MCP arguments exceed schema size bound.");
    };
    if (value.is_object()) {
        size_bounds(value.size(), "minProperties", "maxProperties");
        if (schema.contains("required"))
            for (const auto& name : schema["required"]) require(value.contains(name.get<std::string>()), "Missing required MCP argument.");
        for (const auto& [name, item] : value.items()) {
            if (schema.contains("properties") && schema["properties"].contains(name))
                validate_arguments(item, schema["properties"][name], depth + 1);
            else if (schema.contains("additionalProperties"))
                validate_arguments(item, schema["additionalProperties"], depth + 1);
        }
    } else if (value.is_array()) {
        size_bounds(value.size(), "minItems", "maxItems");
        if (schema.contains("items")) for (const auto& item : value) validate_arguments(item, schema["items"], depth + 1);
    } else if (value.is_string()) {
        // JSON Schema lengths count Unicode code points, rather than UTF-8 bytes.
        const auto& bytes = value.get_ref<const std::string&>();
        const auto count = std::count_if(bytes.begin(), bytes.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
        size_bounds(static_cast<std::size_t>(count), "minLength", "maxLength");
    } else if (value.is_number()) {
        if (schema.contains("minimum")) require(value >= schema["minimum"], "MCP argument is below schema numeric bound.");
        if (schema.contains("maximum")) require(value <= schema["maximum"], "MCP argument exceeds schema numeric bound.");
        if (schema.contains("exclusiveMinimum")) require(value > schema["exclusiveMinimum"], "MCP argument is below schema numeric bound.");
        if (schema.contains("exclusiveMaximum")) require(value < schema["exclusiveMaximum"], "MCP argument exceeds schema numeric bound.");
    }
}

void validate_result(const Json& result) {
    require(result.is_object() && result.contains("content") && result["content"].is_array() &&
            result["content"].size() <= 512, "Invalid MCP tool result.");
    if (result.contains("isError")) require(result["isError"].is_boolean(), "Invalid MCP tool error flag.");
    if (result.contains("structuredContent")) require(result["structuredContent"].is_object(), "Invalid MCP structured result.");
    for (const auto& content : result["content"]) {
        require(content.is_object() && content.contains("type") && content["type"].is_string(), "Invalid MCP content item.");
        const auto type = content["type"].get<std::string>();
        if (type == "text") {
            require(content.contains("text") && content["text"].is_string(), "Invalid MCP text result.");
        } else if (type == "image" || type == "audio") {
            require(content.contains("data") && content["data"].is_string() && content.contains("mimeType") &&
                    content["mimeType"].is_string(), "Invalid MCP media result.");
        } else if (type == "resource_link") {
            require(content.contains("uri") && content["uri"].is_string() && content.contains("name") &&
                    content["name"].is_string(), "Invalid MCP resource link.");
        } else if (type == "resource") {
            require(content.contains("resource") && content["resource"].is_object(), "Invalid MCP resource.");
            const auto& resource = content["resource"];
            require(resource.contains("uri") && resource["uri"].is_string() &&
                    (resource.contains("text") != resource.contains("blob")), "Invalid MCP resource contents.");
            require(resource.contains("text") ? resource["text"].is_string() : resource["blob"].is_string(),
                    "Invalid MCP resource contents.");
        } else invalid("Unsupported MCP content item.");
    }
}

class McpExtension final : public ToolExtension {
public:
    McpExtension(Configuration config, const fs::path& workspace, const std::atomic_bool& cancelled,
                 SchemaBudget& budget, SandboxMode sandbox, std::string bearer)
        : config_(std::move(config)) {
        require(!cancelled.load(), "MCP initialization cancelled.");
        if (config_.http) {
            transport_ = mcp_transport::make_http_transport({config_.url, std::move(bearer), config_.allow_http_loopback});
        } else {
            ProcessRequest request{config_.executable, config_.arguments, workspace, config_.timeout, max_document_bytes};
            request.sandbox = sandbox;
            if (config_.plugin && sandbox!=SandboxMode::none) {
                request.package=config_.plugin->process_package();
                request.package_limits=config_.package_limits;
            }
            transport_ = mcp_transport::make_stdio_transport(request,cancelled);
        }
        const auto initialized = exchange("initialize", {{"protocolVersion", "2025-11-25"},
            {"capabilities", Json::object()}, {"clientInfo", {{"name", "CodeStudio-Agent"}, {"version", "0.1.0"}}}}, cancelled);
        require(initialized.is_object() && initialized.contains("protocolVersion") &&
                (initialized["protocolVersion"] == "2025-11-25" || initialized["protocolVersion"] == "2025-06-18" ||
                 initialized["protocolVersion"] == "2025-03-26"), "Unsupported MCP protocol version.");
        require(initialized.contains("capabilities") && initialized["capabilities"].is_object() &&
                initialized["capabilities"].contains("tools") && initialized["capabilities"]["tools"].is_object(),
                "MCP server does not declare tools support.");
        require(initialized.contains("serverInfo") && initialized["serverInfo"].is_object() &&
                initialized["serverInfo"].contains("name") && initialized["serverInfo"]["name"].is_string() &&
                initialized["serverInfo"].contains("version") && initialized["serverInfo"]["version"].is_string(),
                "Invalid MCP server information.");
        transport_->negotiated(initialized["protocolVersion"].get<std::string>());
        transport_->send(Json({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}),
                       std::chrono::steady_clock::now() + config_.timeout, cancelled);
        discover(cancelled, budget);
    }

    Json schemas() const override { return schemas_; }
    void authorize(const ToolCall& call) const override {
        std::lock_guard guard(mutex_);
        check_call(call);
    }
    ToolResult execute(const ToolCall& call, const std::atomic_bool& cancelled) const override {
        std::lock_guard guard(mutex_);
        try { check_call(call); }
        catch (const std::exception&) { return {false, Json::object(), "MCP tool authorization failed.", "none"}; }
        if (cancelled.load()) return {false, Json::object(), "MCP tool call cancelled before dispatch.", "none"};
        // Process pending notifications before dispatch. The ping also confirms
        // the session is alive without invoking a granted operation.
        try { exchange("ping", Json::object(), cancelled); }
        catch (const std::exception&) {
            return {false, Json::object(), "MCP session stopped before tool dispatch.", "none"};
        }
        try {
            auto result = exchange("tools/call", {{"name", tools_.at(call.name).name}, {"arguments", call.arguments}}, cancelled);
            validate_result(result);
            const bool ok = !result.value("isError", false);
            const auto& output_schema = tools_.at(call.name).output_schema;
            if (ok && !output_schema.is_null()) {
                require(result.contains("structuredContent"), "MCP result is missing its declared structured output.");
                validate_arguments(result["structuredContent"], output_schema);
            }
            Json output{{"content", result["content"]}};
            if (result.contains("structuredContent")) output["structuredContent"] = result["structuredContent"];
            output["isError"] = !ok;
            // A successful protocol response is an execution receipt, not proof
            // that the server's claims or the user's whole task are correct.
            return {ok, std::move(output), ok ? "" : "MCP server reported a tool execution error.", ok ? "applied" : "unknown"};
        } catch (const std::exception&) {
            invalidate();
            return {false, Json::object(), "MCP tool call failed; its effects are unknown. The session was stopped.", "unknown"};
        }
    }

private:
    struct RegisteredTool { std::string name; Json schema; Json output_schema; };
    Configuration config_;
    mutable std::unique_ptr<mcp_transport::Transport> transport_;
    mutable std::mutex mutex_;
    mutable bool valid_ = true;
    mutable std::uint64_t next_id_ = 1;
    std::map<std::string, RegisteredTool> tools_;
    Json schemas_ = Json::array();

    void invalidate() const noexcept {
        valid_ = false;
        if (transport_) transport_->close();
    }
    void check_call(const ToolCall& call) const {
        require(valid_, "MCP session is no longer valid.");
        const auto found = tools_.find(call.name);
        require(found != tools_.end(), "MCP tool is not granted.");
        require(call.arguments.is_object(), "MCP arguments must be an object.");
        require(call.arguments.dump().size() <= max_document_bytes / 2, "MCP arguments exceed size limit.");
        validate_arguments(call.arguments, found->second.schema);
    }
    Json exchange(const std::string& method, const Json& params, const std::atomic_bool& cancelled) const {
        require(valid_, "MCP session is no longer valid.");
        const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
        const auto id = next_id_++;
        try {
            std::size_t bytes = 0;
            std::size_t count = 0;
            std::optional<Json> result;
            transport_->exchange(Json({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}),
                deadline, cancelled, [&](const Json& message) -> mcp_transport::MessageAction {
                require(count++ <= max_side_messages, "MCP server sent too many messages without a response.");
                bytes += message.dump().size();
                require(bytes <= max_document_bytes * 4, "MCP response stream exceeds size limit.");
                require(message.is_object() && message.value("jsonrpc", Json()) == "2.0", "Invalid MCP RPC message.");
                if (message.contains("method")) {
                    require(message["method"].is_string() && !message.contains("result") && !message.contains("error"),
                            "Invalid MCP server request.");
                    if (message.contains("params")) require(message["params"].is_object(), "Invalid MCP server parameters.");
                    if (message["method"] == "notifications/tools/list_changed") {
                        invalid("MCP tool list changed; reload with explicit grants.");
                    }
                    if (message.contains("id")) {
                        require(message["id"].is_number_integer() || message["id"].is_string(), "Invalid MCP request id.");
                        Json response{{"jsonrpc", "2.0"}, {"id", message["id"]}};
                        if (message["method"] == "ping") response["result"] = Json::object();
                        else response["error"] = {{"code", -32601}, {"message", "Client method not supported."}};
                        return {false, std::move(response)};
                    }
                    return {};
                }
                require(message.contains("id") && message["id"].is_number_integer() && message["id"] == id &&
                        (message.contains("result") != message.contains("error")), "Mismatched or malformed MCP RPC response.");
                if (message.contains("error")) invalid("MCP server returned an RPC error.");
                result = message["result"];
                return {true, std::nullopt};
            });
            require(result.has_value(), "MCP transport ended without a response.");
            return std::move(*result);
        } catch (...) {
            invalidate();
            invalid("MCP protocol exchange failed.");
        }
    }
    void discover(const std::atomic_bool& cancelled, SchemaBudget& budget) {
        std::set<std::string> seen_names, cursors;
        Json params = Json::object();
        for (std::size_t page = 0; page < max_pages; ++page) {
            const auto result = exchange("tools/list", params, cancelled);
            require(result.is_object() && result.contains("tools") && result["tools"].is_array(), "Invalid MCP tool list.");
            for (const auto& tool : result["tools"]) {
                require(tool.is_object() && tool.contains("name") && clean_string(tool["name"], 128) &&
                        !tool["name"].get_ref<const std::string&>().empty(), "Invalid MCP listed tool.");
                const auto name = tool["name"].get<std::string>();
                require(seen_names.insert(name).second && seen_names.size() <= max_tools, "Repeated or excessive MCP tools.");
                if (std::find(config_.tools.begin(), config_.tools.end(), name) == config_.tools.end()) continue;
                require(tool.contains("inputSchema") && tool["inputSchema"].is_object() &&
                        tool["inputSchema"].value("type", Json()) == "object", "MCP input schema must describe an object.");
                check_schema(tool["inputSchema"]);
                Json output_schema;
                if (tool.contains("outputSchema")) {
                    require(tool["outputSchema"].is_object() && tool["outputSchema"].value("type", Json()) == "object",
                            "MCP output schema must describe an object.");
                    check_schema(tool["outputSchema"]);
                    output_schema = tool["outputSchema"];
                }
                if (tool.contains("execution")) {
                    require(tool["execution"].is_object(), "Invalid MCP tool execution declaration.");
                    const auto task_support = tool["execution"].value("taskSupport", Json("forbidden"));
                    require(task_support == "forbidden" || task_support == "optional", "MCP task-only or invalid task declarations are not supported.");
                }
                std::string description;
                if (tool.contains("description")) {
                    require(clean_string(tool["description"], 65536), "Invalid MCP tool description.");
                    description = tool["description"].get<std::string>();
                }
                const auto alias = "mcp_" + config_.id + "_" + name;
                Json schema{{"type", "function"}, {"function", {{"name", alias},
                    {"description", description}, {"parameters", tool["inputSchema"]}}}};
                budget.charge(schema, output_schema);
                tools_.emplace(alias, RegisteredTool{name, tool["inputSchema"], std::move(output_schema)});
                schemas_.push_back(std::move(schema));
            }
            if (!result.contains("nextCursor")) {
                require(tools_.size() == config_.tools.size(), "A granted MCP tool is absent from the server tool list.");
                return;
            }
            require(clean_string(result["nextCursor"], 4096) && !result["nextCursor"].get_ref<const std::string&>().empty() &&
                    cursors.insert(result["nextCursor"].get<std::string>()).second, "Invalid or repeated MCP pagination cursor.");
            params = {{"cursor", result["nextCursor"]}};
        }
        invalid("MCP tool discovery exceeds page limit.");
    }
};
}

struct McpPlan::Impl {
    std::string text;
    std::vector<Configuration> configs;
    fs::path root;
    SandboxMode sandbox;
};

McpPlan::McpPlan(const fs::path& manifest, const fs::path& workspace,
                 const std::vector<std::string>& granted_servers, SandboxMode sandbox) {
    validate_sandbox_mode(sandbox);
    auto plan = std::make_shared<Impl>();
    plan->text = read_manifest(manifest);
    plan->configs = configurations(plan->text, granted_servers, extension_tool_limit, sandbox);
    require(fs::is_directory(workspace), "MCP workspace must be a directory.");
    plan->root = fs::canonical(workspace);
    plan->sandbox = sandbox;
    impl_ = std::move(plan);
}

const std::string& McpPlan::snapshot() const { return impl_->text; }

McpOAuthConfig mcp_oauth_configuration(const Json& server) {
    require(server.is_object() && server.contains("oauth") && server.contains("url") &&
            server.value("transport",Json())=="streamable-http" && !server.contains("credential"), "Invalid MCP OAuth server.");
    const auto& oauth = server.at("oauth");
    exact_keys(oauth,{"issuer","client_id","credential","scopes"});
    for (const auto* name : {"issuer","client_id","credential"})
        require(oauth.contains(name) && clean_string(oauth.at(name),8192), "Invalid MCP OAuth configuration.");
    require(oauth.contains("scopes") && oauth.at("scopes").is_array() && oauth.at("scopes").size()<=32,
            "Invalid MCP OAuth scopes.");
    McpOAuthConfig config;
    config.resource=server.at("url").get<std::string>();
    config.issuer=oauth.at("issuer").get<std::string>();
    config.client_id=oauth.at("client_id").get<std::string>();
    config.credential=oauth.at("credential").get<std::string>();
    config.allow_http_loopback=server.value("allow_http_loopback",false);
    config.timeout=std::chrono::milliseconds(server.value("timeout_ms",10000));
    for (const auto& scope : oauth.at("scopes")) {
        require(clean_string(scope,256), "Invalid MCP OAuth scope.");
        config.scopes.push_back(scope.get<std::string>());
    }
    validate_mcp_oauth(config);
    return config;
}

McpOAuthConfig mcp_oauth_configuration(const fs::path& manifest,const std::string& server) {
    const auto configs=configurations(read_manifest(manifest),{server},extension_tool_limit,SandboxMode::none);
    require(configs.size()==1 && configs.front().oauth.has_value(), "Selected MCP server has no OAuth configuration.");
    return *configs.front().oauth;
}

std::vector<McpOAuthConfig> McpPlan::oauth_configurations() const {
    std::vector<McpOAuthConfig> result;
    for (const auto& config : impl_->configs) if (config.oauth) result.push_back(*config.oauth);
    return result;
}
Json McpPlan::plugin_bindings() const {
    Json result=Json::object();
    for (const auto& config : impl_->configs) if (config.plugin) {
        result[config.id]={{"lock",config.plugin->snapshot()}};
        if (impl_->sandbox!=SandboxMode::none)
            result[config.id]["execution"]={{"version",2},{"kind","isolated-process-package"},
                {"processes",config.package_limits.processes},{"memory_bytes",config.package_limits.memory_bytes},
                {"cpu_percent",config.package_limits.cpu_percent}};
    }
    return result;
}
std::vector<fs::path> McpPlan::protected_paths() const {
    std::vector<fs::path> result;
    for (const auto& config : impl_->configs) if (config.plugin) {
        result.push_back(config.plugin_lock);
        result.push_back(config.plugin->directory());
    }
    return result;
}

std::vector<std::string> McpPlan::credential_references() const {
    std::set<std::string> references;
    for (const auto& config : impl_->configs)
        if (!config.credential.empty() && !config.oauth) references.insert(config.credential);
    return {references.begin(), references.end()};
}

std::vector<std::shared_ptr<ToolExtension>> McpPlan::connect(
        const std::function<std::string(const std::string&)>& resolver, const std::atomic_bool& cancelled,
        std::size_t reserved_tools, std::size_t reserved_schema_bytes) const {
    // Resolve every credential before creating any session; one unavailable
    // endpoint must not cause partial launches or requests to other endpoints.
    require(reserved_tools <= extension_tool_limit && reserved_schema_bytes <= extension_schema_byte_limit,
            "Reserved extension schemas exceed the aggregate limit.");
    require((reserved_tools == 0 && reserved_schema_bytes == 0) ||
            (reserved_tools != 0 && reserved_schema_bytes >= 2), "Invalid extension schema reservation.");
    std::size_t granted_count = reserved_tools;
    for (const auto& config : impl_->configs) granted_count += config.tools.size();
    require(granted_count <= extension_tool_limit, "Too many granted MCP tools for the remaining extension budget.");
    require(impl_->configs.empty() || reserved_schema_bytes < extension_schema_byte_limit,
            "No extension schema budget remains for MCP discovery.");
    struct Resolved {
        std::map<std::string, std::string> tokens;
        ~Resolved() { for (auto& [key, value] : tokens) std::fill(value.begin(), value.end(), '\0'); }
    } resolved;
    std::set<std::string> references;
    for (const auto& config : impl_->configs) {
        if (config.plugin) config.plugin->verify();
        if (!config.credential.empty()) references.insert(config.credential);
    }
    for (const auto& reference : references) {
        require(!cancelled.load(), "MCP initialization cancelled.");
        require(static_cast<bool>(resolver), "MCP credential resolver is required.");
        auto token = resolver(reference);
        validate_credential_secret(token);
        resolved.tokens.emplace(reference, std::move(token));
    }
    std::vector<std::shared_ptr<ToolExtension>> result;
    SchemaBudget budget{reserved_tools, reserved_schema_bytes ? reserved_schema_bytes : 2};
    try {
        for (const auto& config : impl_->configs) {
            // Earlier servers have now run; recheck the frozen package just
            // before this server launches as well as at whole-plan preflight.
            if (config.plugin) config.plugin->verify();
            result.push_back(std::make_shared<McpExtension>(config, impl_->root, cancelled, budget, impl_->sandbox,
                config.credential.empty() ? std::string() : resolved.tokens.at(config.credential)));
        }
    } catch (...) {
        result.clear();
        throw;
    }
    return result;
}

std::vector<std::shared_ptr<ToolExtension>> load_mcp_extensions(
        const fs::path& manifest, const fs::path& workspace,
        const std::vector<std::string>& granted_servers, const std::atomic_bool& cancelled,
        SandboxMode sandbox, std::size_t reserved_tools, std::size_t reserved_schema_bytes) {
    return McpPlan(manifest, workspace, granted_servers, sandbox).connect(
        {}, cancelled, reserved_tools, reserved_schema_bytes);
}
}
