#include "extension_commands.hpp"
#include "codestudio/mcp.hpp"
#include "codestudio/plugins.hpp"
#include "codestudio/sandbox_resources.hpp"
#include <charconv>
#include <map>
#include <set>
#include <stdexcept>

namespace codestudio {
namespace {
std::filesystem::path utf8_path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(),value.end()));
}
}
CommandResult extension_command(const std::vector<std::string>& argv,const std::atomic_bool& cancelled,
        const CommandCallbacks& callbacks,const std::function<void(const std::string&)>& remember) {
    const auto& command=argv.at(1);
    const std::map<std::string,std::set<std::string>> accepted{
        {"sandbox-resources",{"--after","--limit"}},
        {"sandbox-reclaim",{"--id","--dry-run"}},
        {"plugin-pack",{"--source","--output"}},
        {"plugin-install",{"--bundle","--url","--sha256","--store","--allow-http-loopback"}},
        {"plugin-lock",{"--directory","--output"}}, {"plugin-inspect",{"--lock"}},
        {"mcp-auth-discover",{"--mcp-config","--server"}}, {"mcp-auth-status",{"--mcp-config","--server"}},
        {"mcp-auth-logout",{"--mcp-config","--server"}},
        {"mcp-auth-login",{"--mcp-config","--server","--login-timeout-ms"}}};
    const auto selected=accepted.find(command);
    if (selected==accepted.end()) throw std::runtime_error("Unknown extension command");
    std::map<std::string,std::string> values;
    for (std::size_t i=2;i<argv.size();++i) {
        const auto& key=argv[i];
        if (!selected->second.contains(key) || values.contains(key)) throw std::runtime_error("Unknown or repeated extension option");
        if (key=="--allow-http-loopback" || key=="--dry-run") { values.emplace(key,"true"); continue; }
        if (i+1==argv.size() || argv[i+1].empty() || argv[i+1].starts_with("--"))
            throw std::runtime_error("Extension option requires a value");
        values.emplace(key,argv[++i]);
    }
    const auto required=[&](const char* name)->const std::string& {
        const auto found=values.find(name);
        if (found==values.end()) throw std::runtime_error(std::string("Missing extension option: ")+name);
        return found->second;
    };
    if (command=="sandbox-resources") {
        unsigned limit=50;
        if (values.contains("--limit")) {
            const auto& text=values.at("--limit");
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),limit);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || limit<1 || limit>100)
                throw std::runtime_error("Sandbox resource limit must be 1..100");
        }
        return {list_sandbox_resources(values.contains("--after") ? values.at("--after") : "",limit),0};
    }
    if (command=="sandbox-reclaim") {
        auto result=reclaim_sandbox_resource(required("--id"),values.contains("--dry-run"),cancelled);
        const auto state=result.at("state").get<std::string>();
        const bool ok=state=="reclaimed" || state=="absent" || state=="reclaimable";
        return {std::move(result),ok ? 0 : 1};
    }
    if (cancelled.load()) throw std::runtime_error("Extension operation cancelled");
    if (command=="plugin-pack") return {pack_plugin(utf8_path(required("--source")),utf8_path(required("--output"))),0};
    if (command=="plugin-install") {
        if (values.contains("--bundle")==values.contains("--url"))
            throw std::runtime_error("plugin-install requires exactly one of --bundle or --url");
        const auto store=utf8_path(required("--store"));
        const auto hash=required("--sha256");
        if (values.contains("--url")) return {install_plugin_url(required("--url"),hash,store,cancelled,values.contains("--allow-http-loopback")),0};
        if (values.contains("--allow-http-loopback")) throw std::runtime_error("Loopback option requires --url");
        return {install_plugin(utf8_path(required("--bundle")),hash,store),0};
    }
    if (command=="plugin-lock") return {lock_plugin(utf8_path(required("--directory")),utf8_path(required("--output"))),0};
    if (command=="plugin-inspect") return {inspect_plugin_lock(utf8_path(required("--lock"))),0};
    const auto config=mcp_oauth_configuration(utf8_path(required("--mcp-config")),required("--server"));
    for (const auto& secret : mcp_oauth_secrets(config)) if (remember) remember(secret);
    if (command=="mcp-auth-discover") return {discover_mcp_oauth(config,cancelled),0};
    if (command=="mcp-auth-status") return {status_mcp_oauth(config,cancelled),0};
    if (command=="mcp-auth-logout") return {logout_mcp_oauth(config,cancelled),0};
    auto timeout=300000;
    if (values.contains("--login-timeout-ms")) {
        const auto& text=values.at("--login-timeout-ms");
        const auto result=std::from_chars(text.data(),text.data()+text.size(),timeout);
        if (result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || timeout<100 || timeout>300000)
            throw std::runtime_error("Login timeout must be 100..300000 milliseconds");
    }
    if (!callbacks.on_authorization) throw std::runtime_error("OAuth login requires an authorization URL callback");
    const auto notice=[&](const Json& value) {
        // Refuse rather than publish a modified authorization URL: metadata
        // must not smuggle a previously saved token through this side channel.
        if (application_redact(value)!=value)
            throw std::runtime_error("OAuth authorization notice contains a configured credential");
        callbacks.on_authorization(value);
    };
    return {login_mcp_oauth(config,notice,cancelled,remember,std::chrono::milliseconds(timeout)),0};
}
}
