#include "oauth_internal.hpp"
#include "codestudio/credentials.hpp"
#include "codestudio/digest.hpp"
#include "../provider/protocol.hpp"
#include <algorithm>
#include <set>
#include <sstream>

namespace codestudio {
namespace {
using namespace oauth_internal;
bool text(const Json& value, std::size_t maximum = 8192) {
    return value.is_string() && !value.get_ref<const std::string&>().empty() &&
        value.get_ref<const std::string&>().size() <= maximum &&
        std::ranges::all_of(value.get_ref<const std::string&>(), [](unsigned char c) { return c > 32 && c < 127; });
}
std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string scope_text(const std::vector<std::string>& scopes) {
    std::string result;
    for (const auto& scope : scopes) { if (!result.empty()) result += ' '; result += scope; }
    return result;
}
Json parse(const std::string& value) {
    try { return protocol::strict_json(value); }
    catch (...) { fail("invalid authorization document"); }
}
std::string credential_name(const McpOAuthConfig& config) { return parse_credential_reference(config.credential).name; }
Json json_response(const Response& response) {
    require(response.status == 200, "authorization endpoint did not return success");
    const auto type = response.headers.find("content-type");
    require(type != response.headers.end() && (type->second == "application/json" || type->second.starts_with("application/json;")),
            "authorization endpoint did not return JSON");
    auto value = parse(response.body);
    require(value.is_object(), "authorization document must be an object");
    return value;
}
bool contains(const Json& value, const std::string& expected) {
    require(value.is_array() && value.size() <= 128, "invalid authorization metadata list");
    bool found = false;
    for (const auto& entry : value) { require(text(entry), "invalid authorization metadata entry"); found |= entry == expected; }
    return found;
}
struct Metadata { std::string authorization, token; bool issuer_parameter = false; };
Metadata discover(const McpOAuthConfig& config, const std::atomic_bool& cancelled) {
    const auto resource = url(config.resource, config.allow_http_loopback);
    const auto issuer = url(config.issuer, config.allow_http_loopback);
    const auto deadline = std::chrono::steady_clock::now() + config.timeout;
    // No access token is sent during discovery. The protected endpoint may
    // advertise a same-origin RFC9728 metadata document in its 401 challenge.
    std::vector<std::string> candidates;
    const auto probe = request(config.resource, config.allow_http_loopback, {}, deadline, cancelled);
    if (probe.status == 401 && probe.headers.contains("www-authenticate")) {
        const auto& challenge = probe.headers.at("www-authenticate");
        const auto marker = challenge.find("resource_metadata=\"");
        if (marker != std::string::npos) {
            const auto start = marker + std::string_view("resource_metadata=\"").size();
            const auto end = challenge.find('"', start);
            require(end != std::string::npos && challenge.find("resource_metadata=", end) == std::string::npos,
                    "ambiguous protected-resource metadata challenge");
            auto endpoint = challenge.substr(start, end - start);
            require(url(endpoint, config.allow_http_loopback).origin == resource.origin,
                    "protected-resource metadata must share the configured resource origin");
            candidates.push_back(std::move(endpoint));
        }
    }
    if (resource.path != "/") candidates.push_back(resource.origin + "/.well-known/oauth-protected-resource" + resource.path);
    candidates.push_back(resource.origin + "/.well-known/oauth-protected-resource");
    Json protected_resource;
    std::set<std::string> visited;
    for (const auto& candidate : candidates) {
        if (!visited.insert(candidate).second) continue;
        const auto response = request(candidate, config.allow_http_loopback, {}, deadline, cancelled);
        if (response.status == 404 || response.status == 405) continue;
        protected_resource = json_response(response); break;
    }
    require(protected_resource.is_object() && protected_resource.value("resource", Json{}) == config.resource,
            "protected-resource metadata resource mismatch");
    require(protected_resource.contains("authorization_servers") && contains(protected_resource["authorization_servers"], config.issuer),
            "configured issuer is not advertised by resource");
    std::vector<std::string> issuer_candidates;
    const auto suffix = issuer.path == "/" ? std::string{} : issuer.path;
    issuer_candidates.push_back(issuer.origin + "/.well-known/oauth-authorization-server" + suffix);
    issuer_candidates.push_back(issuer.origin + "/.well-known/openid-configuration" + suffix);
    if (!suffix.empty()) issuer_candidates.push_back(config.issuer + (config.issuer.ends_with('/') ? "" : "/") + ".well-known/openid-configuration");
    Json authorization;
    for (const auto& candidate : issuer_candidates) {
        const auto response = request(candidate, config.allow_http_loopback, {}, deadline, cancelled);
        if (response.status == 404 || response.status == 405) continue;
        authorization = json_response(response); break;
    }
    require(authorization.is_object() && authorization.value("issuer", Json{}) == config.issuer, "authorization metadata issuer mismatch");
    require(authorization.contains("authorization_endpoint") && text(authorization["authorization_endpoint"]) &&
            authorization.contains("token_endpoint") && text(authorization["token_endpoint"]), "authorization endpoints are missing");
    require(authorization.contains("code_challenge_methods_supported") && contains(authorization["code_challenge_methods_supported"], "S256"),
            "authorization server must advertise PKCE S256");
    require(authorization.contains("response_types_supported") && contains(authorization["response_types_supported"], "code"),
            "authorization server does not support authorization code");
    if (authorization.contains("grant_types_supported")) require(contains(authorization["grant_types_supported"], "authorization_code"), "authorization code grant unsupported");
    if (authorization.contains("token_endpoint_auth_methods_supported")) require(contains(authorization["token_endpoint_auth_methods_supported"], "none"), "public client authentication unsupported");
    Metadata result{authorization["authorization_endpoint"].get<std::string>(), authorization["token_endpoint"].get<std::string>(), false};
    (void)url(result.authorization, config.allow_http_loopback); (void)url(result.token, config.allow_http_loopback);
    if (authorization.contains("authorization_response_iss_parameter_supported")) {
        require(authorization["authorization_response_iss_parameter_supported"].is_boolean(), "invalid issuer response support metadata");
        result.issuer_parameter = authorization["authorization_response_iss_parameter_supported"].get<bool>();
    }
    return result;
}
struct Stored {
    std::string state;
    Json record;
};
Stored read(const McpOAuthConfig& config) {
    const auto value = read_oauth_credential(credential_name(config));
    if (value.state != "available") return {value.state, {}};
    const auto record = parse(value.secret);
    require(record.is_object() && record.size() == 7 && record.contains("version") && record["version"].is_number_integer() && record["version"] == 2 &&
            record.contains("binding") && text(record["binding"], 64) && record["binding"].get_ref<const std::string&>().size() == 64 &&
            record.contains("access_token") && text(record["access_token"], 2048) &&
            record.contains("refresh_token") && record["refresh_token"].is_string() &&
            (record["refresh_token"] == "" || text(record["refresh_token"], 2048)) &&
            record.contains("expires_at") && record["expires_at"].is_number_integer() && record["expires_at"] >= 0 && record["expires_at"] <= INT64_MAX &&
            record.contains("token_endpoint") && text(record["token_endpoint"]) &&
            record.contains("refresh_blocked") && record["refresh_blocked"].is_boolean(), "invalid stored authorization record");
    (void)url(record["token_endpoint"].get<std::string>(), config.allow_http_loopback);
    if (record["binding"] != mcp_oauth_token_key(config).substr(6)) return {"binding_mismatch", record};
    if (record["refresh_blocked"].get<bool>()) return {"refresh_blocked", record};
    return {record["expires_at"].get<std::int64_t>() <= now_seconds() ? "expired" : "available", record};
}
std::vector<std::string> secrets(const Json& record, const McpOAuthRemember& remember = {}) {
    std::vector<std::string> result;
    for (const auto key : {"access_token", "refresh_token"}) {
        if (record.is_object() && record.contains(key) && record[key].is_string() && !record[key].get_ref<const std::string&>().empty()) {
            result.push_back(record[key].get<std::string>()); if (remember) remember(result.back());
        }
    }
    return result;
}
void save(const McpOAuthConfig& config, const Json& record) {
    const auto encoded = record.dump(-1, ' ', true);
    require(encoded.size() <= 2048, "authorization record exceeds secure-store capacity");
    require(write_oauth_credential(credential_name(config), encoded) == "available", "secure authorization storage unavailable or locked");
}
Json token_record(const McpOAuthConfig& config, const std::string& endpoint, const Response& response,
                  const std::string& previous_refresh, const McpOAuthRemember& remember) {
    const auto token = json_response(response);
    // Remember known secret fields before reporting any validation error.
    (void)secrets(token, remember);
    require(token.contains("access_token") && text(token["access_token"], 2048) &&
            token.contains("token_type") && token["token_type"].is_string(), "invalid access-token response");
    auto type = token["token_type"].get<std::string>();
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; });
    require(type == "bearer", "only Bearer access tokens are supported");
    require(token.contains("expires_in") && token["expires_in"].is_number_integer() && token["expires_in"] > 0 && token["expires_in"] <= 31536000,
            "access-token lifetime must be explicit and bounded");
    if (token.contains("scope")) {
        require(token["scope"].is_string() && token["scope"].get_ref<const std::string&>().size() <= 2048, "invalid granted scope");
        require(std::ranges::all_of(token["scope"].get_ref<const std::string&>(), [](unsigned char c) {
            return c == 0x20 || c == 0x21 || (c >= 0x23 && c <= 0x5b) || (c >= 0x5d && c <= 0x7e);
        }), "invalid granted scope characters");
        std::istringstream input(token["scope"].get<std::string>()); std::string item; std::set<std::string> granted;
        while (input >> item) require(granted.insert(item).second, "duplicate granted scope");
        require(granted == std::set<std::string>(config.scopes.begin(), config.scopes.end()), "granted scope differs from explicitly configured scope");
    }
    std::string refresh = previous_refresh;
    if (token.contains("refresh_token")) { require(text(token["refresh_token"], 2048), "invalid refresh token"); refresh = token["refresh_token"].get<std::string>(); }
    return {{"version", 2}, {"binding", mcp_oauth_token_key(config).substr(6)}, {"access_token", token["access_token"]},
            {"refresh_token", refresh}, {"expires_at", now_seconds() + token["expires_in"].get<std::int64_t>()},
            {"token_endpoint", endpoint}, {"refresh_blocked", false}};
}
Json status_result(const McpOAuthConfig& config, const Stored& stored) {
    Json result{{"state", stored.state}, {"credential", config.credential}, {"issuer", config.issuer},
                {"resource", config.resource}, {"scopes", config.scopes}, {"backend", system_credential_backend()}};
    if (stored.record.is_object() && stored.state != "binding_mismatch") {
        result["expires_at"] = stored.record["expires_at"];
        result["refresh_available"] = stored.record["refresh_token"] != "" && stored.state != "refresh_blocked";
    }
    return result;
}
}

void validate_mcp_oauth(const McpOAuthConfig& config) {
    (void)oauth_internal::url(config.resource, config.allow_http_loopback);
    (void)oauth_internal::url(config.issuer, config.allow_http_loopback);
    require(text(Json(config.client_id), 512), "invalid public client identifier");
    const auto reference = parse_credential_reference(config.credential);
    require(reference.kind == "system", "OAuth requires a system credential reference");
    require(config.timeout >= std::chrono::milliseconds(100) && config.timeout <= std::chrono::milliseconds(300000), "invalid authorization timeout");
    require(config.scopes.size() <= 32, "too many configured scopes");
    std::set<std::string> unique;
    for (const auto& scope : config.scopes) {
        require(!scope.empty() && scope.size() <= 128 && unique.insert(scope).second &&
                std::ranges::all_of(scope, [](unsigned char c) { return c == 0x21 || (c >= 0x23 && c <= 0x5b) || (c >= 0x5d && c <= 0x7e); }),
                "invalid or duplicate configured scope");
    }
    require(scope_text(config.scopes).size() <= 2048, "configured scopes exceed limit");
}
std::string mcp_oauth_token_key(const McpOAuthConfig& config) {
    validate_mcp_oauth(config);
    return "oauth:" + sha256_bytes(Json{{"resource", config.resource}, {"issuer", config.issuer},
        {"client_id", config.client_id}, {"credential", config.credential}, {"scopes", config.scopes},
        {"allow_http_loopback", config.allow_http_loopback}}.dump());
}
Json discover_mcp_oauth(const McpOAuthConfig& config, const std::atomic_bool& cancelled) {
    validate_mcp_oauth(config); const auto metadata = discover(config, cancelled);
    return {{"state", "discovered"}, {"issuer", config.issuer}, {"resource", config.resource}, {"client_id", config.client_id},
            {"scopes", config.scopes}, {"authorization_endpoint", metadata.authorization}, {"token_endpoint", metadata.token},
            {"pkce", "S256"}, {"registration", "pre_registered_public_client"}, {"authorization_response_iss_required", metadata.issuer_parameter}};
}
Json login_mcp_oauth(const McpOAuthConfig& config, const McpOAuthNotice& notice, const std::atomic_bool& cancelled,
                    const McpOAuthRemember& remember, std::chrono::milliseconds login_timeout) {
    validate_mcp_oauth(config);
    require(login_timeout >= std::chrono::milliseconds(100) && login_timeout <= std::chrono::milliseconds(600000), "invalid login timeout");
    CredentialLock lock(credential_name(config), std::chrono::steady_clock::now() + config.timeout, cancelled);
    const auto existing = read(config); (void)secrets(existing.record, remember);
    require(existing.state == "missing" || existing.state == "available" || existing.state == "expired" || existing.state == "refresh_blocked",
            "authorization storage unavailable, locked or bound to different configuration");
    const auto metadata = discover(config, cancelled);
    Callback callback;
    const auto verifier = random_secret(), state = random_secret(), redirect = callback.redirect_uri();
    if (remember) remember(verifier);
    const auto authorization = metadata.authorization + "?response_type=code&client_id=" + encode(config.client_id) +
        "&redirect_uri=" + encode(redirect) + "&code_challenge=" + encode(pkce(verifier)) + "&code_challenge_method=S256&state=" + encode(state) +
        "&resource=" + encode(config.resource) + (config.scopes.empty() ? "" : "&scope=" + encode(scope_text(config.scopes)));
    require(authorization.size() <= 16384, "authorization URL exceeds notification limit");
    require(static_cast<bool>(notice), "authorization URL callback required");
    notice({{"type", "authorization"}, {"authorization_url", authorization}, {"redirect_uri", redirect}});
    const auto code = callback.wait(state, config.issuer, metadata.issuer_parameter,
                                   std::chrono::steady_clock::now() + login_timeout, cancelled);
    if (remember) remember(code);
    const auto body = "grant_type=authorization_code&code=" + encode(code) + "&client_id=" + encode(config.client_id) +
        "&redirect_uri=" + encode(redirect) + "&code_verifier=" + encode(verifier) + "&resource=" + encode(config.resource);
    const auto response = request(metadata.token, config.allow_http_loopback, body,
                                  std::chrono::steady_clock::now() + config.timeout, cancelled);
    const auto record = token_record(config, metadata.token, response, {}, remember);
    save(config, record);
    return status_result(config, {"available", record});
}
Json status_mcp_oauth(const McpOAuthConfig& config, const std::atomic_bool& cancelled) {
    validate_mcp_oauth(config);
    CredentialLock lock(credential_name(config), std::chrono::steady_clock::now() + config.timeout, cancelled);
    return status_result(config, read(config));
}
Json logout_mcp_oauth(const McpOAuthConfig& config, const std::atomic_bool& cancelled) {
    validate_mcp_oauth(config);
    CredentialLock lock(credential_name(config), std::chrono::steady_clock::now() + config.timeout, cancelled);
    const auto stored = read(config);
    require(stored.state != "binding_mismatch", "authorization record belongs to another configuration");
    return {{"state", delete_oauth_credential(credential_name(config))}, {"credential", config.credential}, {"server_revocation", "not_requested"}};
}
std::vector<std::string> mcp_oauth_secrets(const McpOAuthConfig& config) {
    validate_mcp_oauth(config);
    return secrets(read(config).record);
}
McpOAuthToken resolve_mcp_oauth(const McpOAuthConfig& config, const std::atomic_bool& cancelled, const McpOAuthRemember& remember) {
    validate_mcp_oauth(config);
    CredentialLock lock(credential_name(config), std::chrono::steady_clock::now() + config.timeout, cancelled);
    auto stored = read(config);
    auto redactions = secrets(stored.record, remember);
    require(stored.state == "available" || stored.state == "expired", "authorization unavailable or requires explicit login");
    if (stored.record["expires_at"].get<std::int64_t>() <= now_seconds() + 30) {
        const auto refresh = stored.record["refresh_token"].get<std::string>();
        require(!refresh.empty(), "access token expires soon and requires explicit login");
        // Persist the uncertain-refresh fence BEFORE dispatch. A crash or
        // failure cannot silently replay a possibly rotated refresh token.
        stored.record["refresh_blocked"] = true; save(config, stored.record);
        const auto endpoint = stored.record["token_endpoint"].get<std::string>();
        const auto body = "grant_type=refresh_token&refresh_token=" + encode(refresh) + "&client_id=" + encode(config.client_id) +
            "&resource=" + encode(config.resource) + (config.scopes.empty() ? "" : "&scope=" + encode(scope_text(config.scopes)));
        const auto response = request(endpoint, config.allow_http_loopback, body,
                                      std::chrono::steady_clock::now() + config.timeout, cancelled);
        stored.record = token_record(config, endpoint, response, refresh, remember);
        auto replacement = secrets(stored.record, remember); redactions.insert(redactions.end(), replacement.begin(), replacement.end());
        save(config, stored.record);
    }
    return {stored.record["access_token"].get<std::string>(), std::move(redactions)};
}
}
