#include "codestudio/application.hpp"
#include "codestudio/kernel.hpp"
#include "codestudio/mcp.hpp"
#include "codestudio/local_models.hpp"
#include "codestudio/workspace_build.hpp"
#include "codestudio/build_proposal.hpp"
#include "codestudio/build_approval.hpp"
#include "codestudio/state_reader.hpp"
#include "codestudio/scheduler.hpp"
#include "codestudio/cron.hpp"
#include "codestudio/credentials.hpp"
#include "extension_commands.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace codestudio {
namespace {
// Retain a possible credential suffix across chunks before exposing any text.
// Presentation is left to the adapter so JSON-RPC clients receive raw UTF-8.
class StreamRedactor {
public:
    StreamRedactor(std::vector<std::string> secrets, std::function<void(std::string_view)> sink)
        : secrets_(std::move(secrets)), sink_(std::move(sink)) {}
    void append(std::string_view text) { pending_.append(text); drain(false); }
    void finish() { drain(true); }
private:
    void emit(std::string_view text) { if (sink_ && !text.empty()) sink_(text); }
    void drain(bool final) {
        auto end = pending_.size();
        if (!final) {
            std::size_t retain=0;
            for (const auto& secret : secrets_) {
                if (secret.empty()) continue;
                auto candidate=std::min(end,secret.size()-1);
                while (candidate && pending_.compare(end-candidate,candidate,secret,0,candidate)!=0) --candidate;
                retain=std::max(retain,candidate);
            }
            end-=retain;
        }
        // Merge overlapping values before emitting anything. An incomplete
        // longer value also retains a complete shorter value inside its prefix.
        std::vector<std::size_t> next;
        next.reserve(secrets_.size());
        for (const auto& secret : secrets_)
            next.push_back(secret.empty() ? std::string::npos : pending_.find(secret));
        const auto earliest=[&]() {
            return static_cast<std::size_t>(std::min_element(next.begin(),next.end())-next.begin());
        };
        // Never split a UTF-8 code point when retaining a credential suffix.
        while (end > 0 && end < pending_.size() &&
               (static_cast<unsigned char>(pending_[end]) & 0xc0) == 0x80) --end;
        std::size_t cursor=0;
        while (!next.empty()) {
            auto index=earliest();
            const auto first=next[index];
            if (first>=end) break;
            auto matched_end=first+1;
            while (next[index]<matched_end) {
                matched_end=std::max(matched_end,next[index]+secrets_[index].size());
                next[index]=pending_.find(secrets_[index],next[index]+1);
                index=earliest();
            }
            if (matched_end>end) { end=first; break; }
            emit(std::string_view(pending_).substr(cursor,first-cursor));
            emit("[REDACTED]");
            cursor=matched_end;
        }
        emit(std::string_view(pending_).substr(cursor,end-cursor));
        pending_.erase(0,end);
    }
    std::vector<std::string> secrets_;
    std::string pending_;
    std::function<void(std::string_view)> sink_;
};

std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr; std::size_t size = 0;
    if (_dupenv_s(&value,&size,name) != 0) throw std::runtime_error("Cannot read application environment");
    const std::string result = value ? value : "";
    std::free(value); return result;
#else
    const auto* value = std::getenv(name); return value ? value : "";
#endif
}

// Each invocation owns its selected credentials through final result/error
// redaction. Worker threads never retain a previous job's resolved secret.
class CredentialScope;
thread_local CredentialScope* current_credentials=nullptr;
class CredentialScope {
public:
    CredentialScope() : previous_(current_credentials) {
        remember(environment("CODESTUDIO_API_KEY"));
        current_credentials=this;
    }
    ~CredentialScope() {
        current_credentials=previous_;
        for (auto& secret : secrets) std::fill(secret.begin(),secret.end(),'\0');
        if (selected) std::fill(selected->second.begin(),selected->second.end(),'\0');
        for (auto& [reference,secret] : mcp) std::fill(secret.begin(),secret.end(),'\0');
    }
    CredentialScope(const CredentialScope&)=delete;
    CredentialScope& operator=(const CredentialScope&)=delete;
    void remember(std::string secret) {
        if (!secret.empty() && std::ranges::find(secrets,secret)==secrets.end()) secrets.push_back(std::move(secret));
    }
    std::vector<std::string> secrets;
    std::optional<std::pair<std::string,std::string>> selected;
    std::map<std::string,std::string> mcp;
private:
    CredentialScope* previous_;
};
template<class Operation> CommandResult with_credential_scope(Operation&& operation) {
    CredentialScope scope;
    try {
        auto result=operation();
        result.value=application_redact(std::move(result.value));
        return result;
    } catch (const RunBusy& error) {
        // Worker polling distinguishes transient lock contention from a failed
        // job. Retain that category while sanitizing its message.
        throw RunBusy(application_redact(error.what()).get<std::string>());
    } catch (const std::filesystem::filesystem_error& error) {
        if (!scope.selected) throw std::runtime_error("Application filesystem operation failed");
        throw std::runtime_error(application_redact(error.what()).get<std::string>());
    } catch (const std::exception& error) {
        // The adapter catches this after the scope is gone, so sanitize here.
        throw std::runtime_error(application_redact(error.what()).get<std::string>());
    }
}
std::string resolve_operation_credential(const std::string& reference_text,const std::atomic_bool& cancelled) {
    const auto reference=parse_credential_reference(reference_text);
    if (!current_credentials) throw std::logic_error("Missing credential operation scope");
    if (cancelled.load()) throw std::runtime_error("Credential resolution cancelled");
    if (current_credentials->selected) {
        if (current_credentials->selected->first!=reference_text)
            throw std::runtime_error("Operation credential reference changed");
        return current_credentials->selected->second;
    }
    auto resolved=read_credential(reference);
    if (resolved.state!="available") {
        if (resolved.state=="missing") throw std::runtime_error("Credential is missing");
        if (resolved.state=="locked") throw std::runtime_error("Credential store is locked");
        throw std::runtime_error("Credential is unavailable");
    }
    current_credentials->remember(resolved.secret);
    validate_credential_secret(resolved.secret);
    if (cancelled.load()) throw std::runtime_error("Credential resolution cancelled");
    current_credentials->selected=std::pair{reference_text,std::move(resolved.secret)};
    return current_credentials->selected->second;
}
std::string saved_credential_reference(const Json& provider) {
    if (!provider.is_object() || !provider.contains("credential") || !provider.at("credential").is_string())
        throw std::runtime_error("Checkpoint has no supported credential reference; create a new task");
    const auto reference=provider.at("credential").get<std::string>();
    (void)parse_credential_reference(reference);
    return reference;
}
void resolve_mcp_credential(const std::string& reference_text,const std::atomic_bool& cancelled) {
    const auto reference=parse_credential_reference(reference_text);
    if (!current_credentials) throw std::logic_error("Missing credential operation scope");
    if (cancelled.load()) throw std::runtime_error("MCP credential resolution cancelled");
    if (current_credentials->mcp.contains(reference_text)) return;
    // MCP references have their own cache. A missing reference never falls back
    // to the model credential, even when that credential has already resolved.
    auto resolved=read_credential(reference);
    if (resolved.state!="available") {
        if (resolved.state=="missing") throw std::runtime_error("MCP credential is missing");
        if (resolved.state=="locked") throw std::runtime_error("MCP credential store is locked");
        throw std::runtime_error("MCP credential is unavailable");
    }
    current_credentials->remember(resolved.secret);
    validate_credential_secret(resolved.secret);
    if (cancelled.load()) throw std::runtime_error("MCP credential resolution cancelled");
    current_credentials->mcp.emplace(reference_text,std::move(resolved.secret));
}
std::string pinned_mcp_credential(const std::string& reference) {
    if (!current_credentials) throw std::logic_error("Missing credential operation scope");
    const auto found=current_credentials->mcp.find(reference);
    if (found==current_credentials->mcp.end())
        throw std::logic_error("MCP credential was not resolved during preflight");
    return found->second;
}
void resolve_mcp_credentials(const McpPlan& plan,const std::atomic_bool& cancelled) {
    for (const auto& reference : plan.credential_references()) resolve_mcp_credential(reference,cancelled);
    // Reject already unusable connections before rotating any other server's
    // token. Resolution rechecks under its lock to handle concurrent changes.
    for (const auto& config : plan.oauth_configurations()) {
        for (auto& secret : mcp_oauth_secrets(config)) current_credentials->remember(std::move(secret));
        const auto state=status_mcp_oauth(config,cancelled).at("state");
        if (state!="available" && state!="expired")
            throw std::runtime_error("MCP OAuth authorization unavailable or requires explicit login");
    }
    for (const auto& config : plan.oauth_configurations()) {
        const auto key=mcp_oauth_token_key(config);
        if (current_credentials->mcp.contains(key)) continue;
        auto token=resolve_mcp_oauth(config,cancelled,[](const std::string& secret) { current_credentials->remember(secret); });
        for (auto& secret : token.secrets) current_credentials->remember(std::move(secret));
        current_credentials->remember(token.access_token);
        current_credentials->mcp.emplace(key,std::move(token.access_token));
    }
}
void remember_mcp_credentials(const McpPlan& plan,const std::atomic_bool& cancelled) {
    for (const auto& reference : plan.credential_references()) resolve_mcp_credential(reference,cancelled);
    for (const auto& config : plan.oauth_configurations()) {
        if (cancelled.load()) throw std::runtime_error("MCP credential preflight cancelled");
        auto secrets=mcp_oauth_secrets(config);
        if (secrets.empty()) throw std::runtime_error("MCP OAuth credentials are unavailable; log in explicitly");
        for (auto& secret : secrets) current_credentials->remember(std::move(secret));
    }
}
void resolve_saved_mcp_credentials(const Json& configuration,const std::atomic_bool& cancelled) {
    if (!configuration.is_object() || !configuration.contains("mcp"))
        throw std::runtime_error("Checkpoint has no supported MCP configuration");
    const auto& binding=configuration.at("mcp");
    if (binding.is_null()) return;
    if (!binding.is_object() || !binding.contains("manifest") || !binding["manifest"].is_string() ||
        binding["manifest"].get_ref<const std::string&>().size()>1024*1024 ||
        !binding.contains("grants") || !binding["grants"].is_array() || binding["grants"].size()>32)
        throw std::runtime_error("Checkpoint has an invalid MCP credential binding");
    // Approval only needs the exact saved credential references for output
    // redaction. It must neither reconnect nor reread a mutable manifest.
    const auto document=Json::parse(binding.at("manifest").get_ref<const std::string&>(),nullptr,false);
    if (!document.is_object() || !document.contains("version") || document["version"]!=2 ||
        !document.contains("servers") || !document["servers"].is_array() || document["servers"].size()>32)
        throw std::runtime_error("Checkpoint has an invalid MCP credential binding");
    std::set<std::string> grants,ids,references;
    for (const auto& grant : binding.at("grants")) {
        if (!grant.is_string() || !grants.insert(grant.get<std::string>()).second)
            throw std::runtime_error("Checkpoint has invalid MCP grants");
    }
    for (const auto& server : document.at("servers")) {
        if (!server.is_object() || !server.contains("id") || !server["id"].is_string() ||
            !ids.insert(server["id"].get<std::string>()).second)
            throw std::runtime_error("Checkpoint has an invalid MCP server binding");
        if (!grants.contains(server["id"].get<std::string>())) continue;
        const auto transport=server.value("transport",Json("stdio"));
        if (transport!="stdio" && transport!="streamable-http")
            throw std::runtime_error("Checkpoint has an unsupported MCP transport");
        if (server.contains("oauth")) {
            auto secrets=mcp_oauth_secrets(mcp_oauth_configuration(server));
            if (secrets.empty()) throw std::runtime_error("Saved MCP OAuth credentials are unavailable for approval redaction");
            for (auto& secret : secrets)
                current_credentials->remember(std::move(secret));
        }
        if (!server.contains("credential")) continue;
        if (transport!="streamable-http" || !server["credential"].is_string())
            throw std::runtime_error("Checkpoint has an invalid MCP credential reference");
        const auto reference=server["credential"].get<std::string>();
        (void)parse_credential_reference(reference);
        references.insert(reference);
    }
    for (const auto& grant : grants) if (!ids.contains(grant))
        throw std::runtime_error("Checkpoint MCP grant names an absent server");
    for (const auto& reference : references) resolve_mcp_credential(reference,cancelled);
}
void validate_mcp_schema_secrets(const std::vector<std::shared_ptr<ToolExtension>>& extensions) {
    for (const auto& extension : extensions) {
        const auto schemas=extension->schemas();
        const auto serialized=schemas.dump();
        if (application_redact(schemas)!=schemas || application_redact(serialized)!=serialized)
            throw std::runtime_error("MCP tool schemas contain a configured credential");
    }
}
struct Arguments {
    std::string command;
    std::map<std::string,std::string> values;
    std::set<std::string> flags;
    std::vector<std::string> executables;
    std::vector<std::string> mcp_servers;
    std::string task_id;
    std::string get(const std::string& name, const std::string& fallback = "") const {
        const auto it = values.find(name); return it == values.end() ? fallback : it->second;
    }
    bool has(const std::string& name) const { return flags.contains(name); }
    int number(const std::string& name, int fallback, int low, int high) const {
        const auto value = get(name,std::to_string(fallback)); int result = 0;
        const auto [ptr,err] = std::from_chars(value.data(),value.data()+value.size(),result);
        if (err != std::errc{} || ptr != value.data()+value.size() || result < low || result > high)
            throw std::runtime_error(name + " must be in " + std::to_string(low) + ".." + std::to_string(high));
        return result;
    }
};
Arguments parse(const std::vector<std::string>& args) {
    Arguments parsed;
    parsed.command = args.size() > 1 ? args[1] : "help";
    if (parsed.command == "--help" || parsed.command == "-h") parsed.command = "help";
    if (parsed.command == "--version") parsed.command = "version";
    const std::set<std::string> commands{"help","version","doctor","demo","build","build-review","build-approve","build-reject","run","resume","schedule-resume","cron-create","history","show","recover","models","model-check","mcp-tools"};
    if (!commands.contains(parsed.command)) throw std::runtime_error("Unknown command: " + parsed.command);
    const std::set<std::string> common{"--workspace","--state"};
    const std::set<std::string> run_values{"--prompt","--base-url","--model","--api","--credential","--max-output-tokens","--max-turns","--timeout-ms","--process-timeout-ms"};
    const std::set<std::string> run_flags{"--allow-write","--allow-http-loopback","--no-stream","--allow-build-proposals"};
    const std::set<std::string> local_values{"--base-url","--model","--api","--timeout-ms"};
    const bool local = parsed.command == "models" || parsed.command == "model-check";
    const bool decision = parsed.command == "build-approve" || parsed.command == "build-reject";
    const bool continuing = decision && std::ranges::find(args,std::string_view("--continue-task")) != args.end();
    const bool scheduling = parsed.command == "schedule-resume";
    const bool recurring = parsed.command == "cron-create";
    const bool executing = parsed.command == "run" || parsed.command == "resume" || scheduling || recurring || continuing;
    const bool proposal = parsed.command == "build-review" || parsed.command == "build-approve" || parsed.command == "build-reject";
    const bool mcp = executing || parsed.command == "mcp-tools";
    for (std::size_t i=2;i<args.size();++i) {
        const auto& key = args[i];
        if ((parsed.command == "show" || parsed.command == "resume" || scheduling || proposal) && key.rfind("--",0) != 0 && parsed.task_id.empty()) {
            parsed.task_id=key; continue;
        }
        if ((decision && key == "--continue-task") || (executing && run_flags.contains(key)) ||
            (local && (key == "--use-api-key" || (parsed.command == "model-check" && key == "--no-stream")))) {
            if (!parsed.flags.insert(key).second) throw std::runtime_error("Repeated option: " + key);
            continue;
        }
        const bool allowed = (!local && common.contains(key)) ||
            (scheduling && key=="--delay-ms") ||
            ((scheduling || recurring) && (key=="--deadline-ms" || key=="--max-attempts" || key=="--retry-delay-ms")) ||
            (recurring && (key=="--cron" || key=="--timezone" || key=="--name" || key=="--max-runs")) ||
            ((executing || parsed.command == "build") && key == "--build-config") ||
            (proposal && (key == "--action" || (parsed.command != "build-review" && key == "--digest"))) ||
            (executing && ((run_values.contains(key) && (parsed.command == "run" || recurring || key != "--prompt")) || key == "--allow-process")) ||
            (mcp && (key == "--mcp-config" || key == "--allow-mcp" || key == "--sandbox")) ||
            (local && local_values.contains(key) && (parsed.command == "model-check" || (key != "--model" && key != "--api")));
        if (!allowed || i+1==args.size() || args[i+1].rfind("--",0)==0)
            throw std::runtime_error("Unknown option or missing value: " + key);
        const auto value = args[++i];
        if (key == "--allow-process") parsed.executables.push_back(value);
        else if (key == "--allow-mcp") parsed.mcp_servers.push_back(value);
        else if (!parsed.values.emplace(key,value).second) throw std::runtime_error("Repeated option: " + key);
    }
    if (parsed.values.contains("--credential")) (void)parse_credential_reference(parsed.get("--credential"));
    if ((parsed.command=="show" || parsed.command=="resume" || scheduling || proposal) && parsed.task_id.empty())
        throw std::runtime_error(parsed.command + " requires a task ID");
    if (proposal && (!parsed.values.contains("--action") || !parsed.values.contains("--workspace")))
        throw std::runtime_error(parsed.command + " requires --action and --workspace");
    if (proposal && parsed.command != "build-review") {
        const auto digest = parsed.get("--digest");
        if (digest.size() != 64 || !std::ranges::all_of(digest, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            throw std::runtime_error("Build decisions require the exact lowercase SHA256 --digest from build-review");
    }
    if (recurring) {
        if (!parsed.values.contains("--workspace") || !parsed.values.contains("--cron"))
            throw std::runtime_error("cron-create requires --workspace and --cron");
        parsed.values["--timezone"]=cron_timezone(parsed.get("--timezone","local"));
        validate_cron(parsed.get("--cron"),parsed.get("--timezone"));
        (void)parsed.number("--max-runs",0,0,1000000);
        (void)parsed.number("--deadline-ms",86400000,1,604800000);
        (void)parsed.number("--max-attempts",8,1,100);
        (void)parsed.number("--retry-delay-ms",1000,100,60000);
        const auto name=parsed.get("--name","Scheduled task");
        if (name.empty() || name.size()>128 || name.find('\0')!=std::string::npos)
            throw std::runtime_error("Cron name must contain 1..128 bytes without NUL");
    }
    return parsed;
}
std::string help_text() {
    return R"(CodeStudio-Agent 0.1.0 — C++26 + XMake

Usage:
  codestudio-agent doctor
  codestudio-agent sandbox-resources [--after <id>] [--limit 50]
  codestudio-agent sandbox-reclaim --id <id> [--dry-run]
  codestudio-agent credential-set --name <id> --stdin
  codestudio-agent credential-delete --name <id>
  codestudio-agent credential-status --credential env:<variable>|system:<id>
  codestudio-agent serve --workspace <directory> [--state <database>]
  codestudio-agent models [--base-url <http://127.0.0.1:11434/v1>] [--timeout-ms 5000]
      [--use-api-key]
  codestudio-agent model-check --model <local-model> [--base-url <loopback-url>]
      [--api chat-completions|responses|messages] [--no-stream] [--timeout-ms 60000]
      [--use-api-key]
  codestudio-agent mcp-tools --workspace <directory> --mcp-config <manifest.json>
      --allow-mcp <server-id>... [--sandbox none|appcontainer|linux-namespace]
  codestudio-agent mcp-auth-discover|mcp-auth-status|mcp-auth-logout
      --mcp-config <manifest.json> --server <id>
  codestudio-agent mcp-auth-login --mcp-config <manifest.json> --server <id>
      [--login-timeout-ms 300000]
  codestudio-agent plugin-pack --source <directory> --output <bundle.csplugin>
  codestudio-agent plugin-install --bundle <file>|--url <https-url>
      --sha256 <digest> --store <directory> [--allow-http-loopback]
  codestudio-agent plugin-lock --directory <installed-release> --output <lock.json>
  codestudio-agent plugin-inspect --lock <lock.json>
  codestudio-agent demo --workspace <new-or-existing-directory>
  codestudio-agent build --workspace <directory> --build-config <manifest.json>
      [--state <database>]
  codestudio-agent build-review <task-id> --action <action-id> --workspace <directory>
      [--state <database>]
  codestudio-agent build-approve|build-reject <task-id> --action <action-id>
      --digest <reviewed-sha256> --workspace <directory> [--state <database>]
      [--continue-task <original resume grants/options>]
  codestudio-agent run --workspace <directory> --prompt <text>
      --base-url <https://host/v1> --model <model>
      [--api chat-completions|responses|messages] [--no-stream]
      [--credential env:<variable>|system:<id>]
      [--max-output-tokens 4096]
      [--allow-write] [--allow-process <absolute-executable>]...
      [--sandbox none|appcontainer|linux-namespace]
      [--max-turns 8] [--timeout-ms 60000] [--process-timeout-ms 10000]
      [--allow-http-loopback] [--state <database>]
      [--mcp-config <manifest.json> --allow-mcp <server-id>...]
      [--build-config <manifest.json>]
      [--allow-build-proposals]
  codestudio-agent resume <task-id> --workspace <directory>
      [--max-turns <total-budget>] [--state <database>]
      [--credential <same-reference-as-original>]
      [--allow-write] [--allow-process <absolute-executable>]...
      [--sandbox none|appcontainer|linux-namespace]
      [--allow-http-loopback] [--mcp-config <manifest.json> --allow-mcp <id>...]
      [--build-config <manifest.json>]
      [--allow-build-proposals]
  codestudio-agent history [--workspace <directory>] [--state <database>]
  codestudio-agent show <task-id> [--workspace <directory>] [--state <database>]
  codestudio-agent recover [--workspace <directory>] [--state <database>]
  codestudio-agent schedule-resume <task-id> --workspace <directory>
      <original resume grants/options> [--state <database>]
      [--delay-ms 0] [--deadline-ms 86400000] [--max-attempts 8]
      [--retry-delay-ms 1000]
  codestudio-agent schedule-list|schedule-show <job-id>|schedule-cancel <job-id>
      --workspace <directory> [--state <database>]
  codestudio-agent scheduler-start|scheduler-run --workspace <directory>
      [--state <database>] [--poll-ms 100] [--once (scheduler-run only)]
  codestudio-agent scheduler-supervise --workspace <directory> [--state <database>]
      [--poll-ms 100] [--max-restarts 3] [--restart-delay-ms 1000]
  codestudio-agent scheduler-start --supervise --workspace <directory>
      [--state <database>] [--poll-ms 100] [--max-restarts 3] [--restart-delay-ms 1000]
  codestudio-agent scheduler-status|scheduler-stop --workspace <directory>
  codestudio-agent scheduler-supervisor-status --workspace <directory> [--state <database>]
  codestudio-agent scheduler-supervisor-events --workspace <directory> [--after <seq>] [--limit <1..100>]
      [--state <database>]
  codestudio-agent cron-create --workspace <directory> --cron "0 9 * * 1-5"
      --prompt <text> <run configuration/grants> [--timezone local|UTC|IANA-zone]
      [--name <name>] [--max-runs 0] [--deadline-ms 86400000]
      [--max-attempts 8] [--retry-delay-ms 1000]
  codestudio-agent cron-list|cron-show <id>|cron-pause <id>|cron-resume <id>|cron-cancel <id>
      --workspace <directory> [--state <database>]
  codestudio-agent cron-preview --workspace <directory> --cron "0 9 * * 1-5"
      [--timezone local|UTC|IANA-zone] [--count 5] [--after <epoch-ms>]
  codestudio-agent scheduler-autostart-enable|scheduler-autostart-status|scheduler-autostart-disable
      --workspace <directory> [--state <database>] [--poll-ms 100 (enable only)]
      [--supervise [--max-restarts 3] [--restart-delay-ms 1000] (enable only)]

serve uses newline-delimited JSON-RPC over stdio for a local desktop client.
It opens no network listener; requests use the same task policy as the CLI.
build executes one explicitly granted build manifest through the action ledger,
without calling a model. run/resume --build-config exposes build_workspace with
empty arguments; the frozen manifest owns its inputs, command and export policy.
The build backend always requires OS isolation and never falls back to none.
Manifest v2 declares explicit steps and environment for the host to validate,
isolate and execute. Build planner plugins have been removed; old plugin plans
require a new explicit manifest/proposal and a new review before execution.
Repeat --build-config when resuming; changed build bindings block continuation.
--allow-build-proposals exposes propose_build for v2 plans without executing them.
Review the saved action with build-review, then explicitly approve or reject its
exact digest. Approval creates one isolated execution task; repeat approval only
returns that task. Interrupted approved builds require recover and never replay.
Approval tasks have no model checkpoint; another attempt needs a new proposal.
Successful proposals pause their source task as waiting_approval (exit 3).
After a decision, resume delivers the decision/build receipt to that same tool
call. --continue-task also resumes a source still waiting on this exact proposal;
repeat its original grants. It retains the saved total model request budget.
Repeated combined decisions never resume an already consumed or later proposal.
schedule-resume persists explicit grants for an existing task, retaining its total
model budget. The independent scheduler waits for due times and build decisions,
retries only safe model-request failures within attempt/deadline/budget limits,
and never approves a proposal or replays uncertain effects. Each run/resume reads
its bound credential reference, then keeps that value for the operation. Only the
reference is stored in task bindings and the queue. scheduler-start runs a
hidden independent Windows process; scheduler-run runs the same loop directly.
Opt-in scheduler-supervise (or scheduler-start --supervise) owns a child worker,
restarts abnormal exits up to --max-restarts (0..100), and backs off from
--restart-delay-ms (100..60000) exponentially to 60000ms. Stops fence restarts.
Supervision does not expand task permissions, reset budgets or replay unknown effects.
Stopping cancels the active job and preserves other queued jobs. Every execution
rechecks current bindings. State databases must use schema 7; no migration occurs.
cron-create freezes a fresh-task template and granted tool schemas without a model
request. Each occurrence gets its own task and budget; max-runs 0 repeats until
paused or cancelled. Missed triggers coalesce, instances never overlap, and
uncertain effects pause the series. Cron uses five numeric fields and an explicit
persisted timezone. Login autostart registers only the current user, never stores
passwords or starts a worker immediately. Environment references use the login
environment, not temporary variables in the shell that enabled autostart.
System references use the current user's available, unlocked native credential
store. There is no plaintext fallback or automatic unlock. System-key rotation
affects later jobs without restarting the worker; in-flight jobs keep their key.

Environment: CODESTUDIO_API_KEY, CODESTUDIO_BASE_URL, CODESTUDIO_MODEL, CODESTUDIO_API.
Model tasks default to --credential env:CODESTUDIO_API_KEY; an explicit
env:<variable> or system:<id> is saved in the exact provider binding. Resume uses
that same reference and rejects a different reference before resolving a key.
Missing, locked or unavailable credentials block execution. credential-set reads
the secret from stdin; status never returns it. Stored values are 1..2048 visible
ASCII bytes. Authentication errors do not authorize replay or reset task budgets.
models/model-check only connect to literal loopback addresses. They ignore cloud
configuration and use no key unless --use-api-key is explicitly supplied.
MCP servers only connect for explicit --allow-mcp IDs and expose only the tools
allowlisted in the frozen manifest. stdio launches a child; streamable-http uses
the configured endpoint and optional explicit credential reference, never the
model key as a fallback. Remote MCP requires --sandbox none; child sandboxes do
not isolate a remote server. Remote HTTP loopback needs allow_http_loopback in
that server's manifest entry. OAuth and legacy HTTP+SSE are not supported.
Streaming is enabled by default. Provisional text goes to stderr; stdout remains
the final JSON receipt. messages selects Anthropic Messages with x-api-key auth.
--max-output-tokens controls the Responses/Messages output token budget.
API keys are never accepted as command-line arguments. Loopback HTTP requires
--allow-http-loopback; remote URLs require HTTPS. No cloud call is made by demo.
State defaults to <workspace>/.codestudio/state.sqlite3. recover only records
interrupted outcomes: it never resumes tools or retries uncertain effects.
resume continues a checkpoint with the same model, endpoint and tool policy.
Repeat the original grants and --sandbox mode. Run recover first after a crash.
Unknown effects, changed configuration and redacted checkpoints block resume. --max-turns is a
total request budget across resumes; increase it explicitly after exhaustion.
--sandbox none is the default and runs children with your user privileges.
Windows --sandbox appcontainer uses LPAC with no network capabilities. It stages
a single standalone executable into private storage with a disposable scratch
directory. Its single-process Job blocks direct descendants and grants no direct workspace access,
and copies no scripts or private DLLs. Host file tools keep their permissions; provider HTTP remains in
the host. Unsupported isolation or launch failure never falls back to none.
Linux --sandbox linux-namespace uses Bubblewrap namespaces, seccomp and a
delegated cgroup v2 with cpu/memory/pids controllers. It stages a native ELF64
tool and its system libraries, denies network and direct descendants, and grants
only private scratch access. See docs/linux.md for runtime requirements.
Sandbox scratch files do not survive process exit or checkpoint resume.
File writes through the host tool are off by default.
The demo grants writes to CODESTUDIO_DEMO.txt, then reads it back for verification.
completed means the model loop ended, not independently certified task success.
)";
}
Json resume_arguments(const Arguments& args,const ToolPolicy& policy,
                      const std::filesystem::path& workspace,const std::filesystem::path& database,
                      const std::string& task) {
    Json result=Json::array({"codestudio-agent","resume",task,
        "--workspace",path_utf8(workspace),"--state",path_utf8(database)});
    const std::set<std::string> registration{"--workspace","--state","--prompt","--delay-ms","--deadline-ms",
        "--max-attempts","--retry-delay-ms","--cron","--timezone","--name","--max-runs"};
    for (const auto& [name,value] : args.values) {
        if (registration.contains(name)) continue;
        result.push_back(name);
        result.push_back(name=="--mcp-config" || name=="--build-config"
            ? path_utf8(std::filesystem::canonical(utf8_path(value))) : value);
    }
    for (const auto& flag : args.flags) result.push_back(flag);
    for (const auto& executable : policy.allowed_executables) {
        result.push_back("--allow-process"); result.push_back(path_utf8(executable));
    }
    for (const auto& server : args.mcp_servers) {
        result.push_back("--allow-mcp"); result.push_back(server);
    }
    if (application_redact(result)!=result)
        throw std::runtime_error("Scheduled authorization requires credential redaction and cannot be persisted");
    return result;
}
void resume_preflight(const Json& task, const Json& checkpoint, const std::filesystem::path& workspace) {
    const auto status=task.at("status").get<std::string>();
    if (status=="running") throw std::runtime_error("Run recover before resuming an interrupted running task");
    if (status!="queued" && status!="interrupted" && status!="cancelled" && status!="failed" && status!="exhausted" && status!="waiting_approval")
        throw std::runtime_error("This task state cannot be resumed");
    if (std::filesystem::weakly_canonical(utf8_path(task.at("workspace").get<std::string>())) != workspace)
        throw std::runtime_error("Resume workspace does not match the original task");
    if (checkpoint.value("version",0)!=2 || !checkpoint.contains("state") || !checkpoint["state"].is_object() ||
        !checkpoint.contains("binding") || !checkpoint["binding"].is_object())
        throw std::runtime_error("Unsupported resume checkpoint");
    const auto& state=checkpoint["state"];
    if (!state.value("resumable",false) || state.value("phase","")=="finished")
        throw std::runtime_error("Checkpoint cannot be resumed; inspect its blocked reason");
    for (const auto& action : task.at("actions")) {
        const auto effect=action.value("effect","");
        const auto action_status=action.value("status","");
        if (effect=="unknown" || effect=="partial" || action_status=="prepared" ||
            action_status=="dispatched" || action_status=="unknown")
            throw std::runtime_error("Task has unresolved tool effects; resume is blocked");
    }
}

// A stateless local coordinator, not a model or shell. Recovering from a safe
// checkpoint uses the existing tool receipt instead of dispatching twice.
class IsolatedBuildProvider final : public ModelProvider {
public:
    std::string name() const override { return "isolated-build"; }
    ModelReply complete(const Json& messages, const Json&, const std::atomic_bool& cancelled) override {
        if (cancelled.load()) throw std::runtime_error("Build request cancelled");
        const Json* receipt = nullptr;
        for (const auto& item : messages) {
            if (item.is_object() && item.value("role", "") == "tool" &&
                item.value("tool_call_id", "") == "isolated_build_1") receipt = &item;
        }
        ModelReply reply;
        if (!receipt) {
            reply.calls.push_back({"isolated_build_1", "build_workspace", Json::object()});
            reply.message = {{"role", "assistant"}, {"content", nullptr},
                {"tool_calls", Json::array({{{"id", "isolated_build_1"}, {"type", "function"},
                    {"function", {{"name", "build_workspace"}, {"arguments", "{}"}}}}})}};
            return reply;
        }
        const auto result = Json::parse(receipt->at("content").get<std::string>());
        if (!result.is_object() || !result.contains("ok") || !result["ok"].is_boolean())
            throw std::runtime_error("Build tool returned an invalid receipt");
        reply.text = result["ok"].get<bool>()
            ? "Isolated build completed; inspect the tool receipt for outputs and execution evidence."
            : "Isolated build did not succeed; inspect the tool receipt before continuing.";
        reply.message = {{"role", "assistant"}, {"content", reply.text}};
        return reply;
    }
};
} // namespace

Json application_redact(Json value) {
    if (current_credentials) return redact(std::move(value),current_credentials->secrets);
    return redact(std::move(value), {environment("CODESTUDIO_API_KEY")});
}

static CommandResult execute_impl(const std::vector<std::string>& argv,
                                 const std::atomic_bool& cancelled,
                                 const CommandCallbacks& callbacks,
                                 Store* inherited_store = nullptr, const Json& scheduled = Json()) {
    auto args = parse(argv);
    if (args.command=="help") return {help_text(), 0};
    if (args.command=="version") return {"CodeStudio-Agent 0.1.0\n", 0};
    if (args.command=="doctor") {
        return {{{"product","CodeStudio-Agent"},{"version","0.1.0"},{"cplusplus",__cplusplus},
#ifdef _MSC_VER
               {"msvc_language_mode",_MSVC_LANG},
#endif
#ifdef __clang__
               {"compiler",std::string("Clang ")+__clang_version__},
#elif defined(_MSC_VER)
               {"compiler","MSVC " + std::to_string(_MSC_VER)},
#else
               {"compiler",__VERSION__},
#endif
               {"build_system","XMake"},{"provider_apis",Json::array({"chat-completions","responses","messages"})},
               {"streaming",true},{"build_manifest_versions",Json::array({2})},{"build_proposals",true},
               {"credentials",{{"references",Json::array({"env","system"})},
                    {"system_backend",system_credential_backend()},{"runtime_probe","not-run"},
                    {"resolution","per-operation"}}},
               {"task_service",{{"transport","stdio"},{"protocol_version",2},{"event_replay",true}}},
               {"mcp_transports",Json::array({"stdio","streamable-http"})},{"local_model_discovery",true},
               {"mcp_oauth",{{"grant","authorization_code"},{"pkce","S256"},{"client_registration","explicit"},
                    {"token_store","system"},{"refresh","before-operation"}}},{"plugin_locks",true},{"plugin_distribution",true},
#ifdef _WIN32
               {"plugin_sandbox",{{"available",true},{"payload","whole-package"},{"workspace_access","none"},
                    {"network",false},{"runtime_probe","not-run"}}},
               {"os_sandbox",true},{"sandbox_backends",Json::array({"appcontainer"})},
               {"workspace_build",{{"available",true},{"backend","appcontainer"},{"runtime_probe","not-run"}}},
#elif defined(__linux__)
               {"plugin_sandbox",{{"available",true},{"payload","whole-package"},{"workspace_access","none"},
                    {"network",false},{"runtime_probe","not-run"}}},
               {"os_sandbox",true},{"sandbox_backends",Json::array({"linux-namespace"})},
               {"workspace_build",{{"available",true},{"backend","linux-namespace"},{"runtime_probe","not-run"}}},
#else
               {"plugin_sandbox",{{"available",false},{"runtime_probe","not-run"}}},
               {"os_sandbox",false},{"sandbox_backends",Json::array()},
               {"workspace_build",{{"available",false},{"backend",nullptr},{"runtime_probe","not-run"}}},
#endif
               {"sandbox_resources",{{"available",
#if defined(_WIN32) || defined(__linux__)
                    true
#else
                    false
#endif
                    },{"reclaim","explicit-id"},{"runtime_probe","not-run"}}},
               {"sandbox_default","none"},{"sandbox_support","compiled-backends"},
               {"sandbox_runtime_probe","not-run"},{"sandbox_scope","process-and-mcp-children"},
               {"automatic_resume",true},{"background_scheduler",true},{"cron",true},{"state_schema",7},
#if defined(_WIN32) || defined(__linux__)
               {"supervised_autostart",true},
               {"worker_supervision",true},{"supervisor_diagnostics",true},
               {"login_autostart",true},
#else
               {"supervised_autostart",false},
               {"worker_supervision",false},{"supervisor_diagnostics",false},
               {"login_autostart",false},
#endif
               {"checkpoint_resume",true},{"approval_continuation",true}}, 0};
    }
    if (args.command=="models" || args.command=="model-check") {
        ProviderConfig config;
        config.base_url=args.get("--base-url","http://127.0.0.1:11434/v1");
        config.api_key=args.has("--use-api-key") ? environment("CODESTUDIO_API_KEY") : "";
        config.model=args.get("--model");
        config.api=parse_provider_api(args.get("--api","chat-completions"));
        config.allow_insecure_loopback=true;
        config.stream=!args.has("--no-stream");
        config.timeout=std::chrono::milliseconds(args.number("--timeout-ms",args.command=="models" ? 5000 : 60000,100,300000));
        const auto result=args.command=="models" ? list_local_models(config,cancelled) : probe_local_model(config,cancelled);
        return {redact(result,{config.api_key}),
                args.command=="model-check" && result.value("status", "") != "compatible" ? 1 : 0};
    }
    // Fresh model operations can select their credential before filesystem
    // diagnostics, which may themselves contain user-supplied secret text.
    if (args.command=="run" || args.command=="cron-create")
        (void)resolve_operation_credential(args.get("--credential","env:CODESTUDIO_API_KEY"),cancelled);
    const auto sandbox_name=args.get("--sandbox","none");
    if (sandbox_name!="none" && sandbox_name!="appcontainer" && sandbox_name!="linux-namespace")
        throw std::runtime_error("--sandbox must be none, appcontainer or linux-namespace");
    const auto sandbox=sandbox_name=="appcontainer" ? SandboxMode::appcontainer :
        sandbox_name=="linux-namespace" ? SandboxMode::linux_namespace : SandboxMode::none;
    validate_sandbox_mode(sandbox);
    if ((args.command=="run" || args.command=="resume" || args.command=="schedule-resume" || args.command=="demo" || args.command=="build" || args.command=="mcp-tools") && !args.values.contains("--workspace"))
        throw std::runtime_error("An explicit --workspace is required");
    if (args.command=="build" && !args.values.contains("--build-config"))
        throw std::runtime_error("build requires an explicit --build-config");
    std::filesystem::path workspace;
    try {
        workspace=std::filesystem::absolute(utf8_path(args.get("--workspace",".")));
        if(args.command=="demo") std::filesystem::create_directories(workspace);
        workspace=std::filesystem::canonical(workspace);
        if(!std::filesystem::is_directory(workspace)) throw std::runtime_error("Workspace must be a directory");
    } catch (const std::filesystem::filesystem_error&) {
        // Resume has not resolved its saved credential yet. Avoid echoing a
        // user-provided path before its eventual secret can be redacted.
        throw std::runtime_error("Workspace path cannot be opened");
    }
    if (!args.mcp_servers.empty() && !args.values.contains("--mcp-config"))
        throw std::runtime_error("--allow-mcp requires --mcp-config");
    if (args.command=="mcp-tools") {
        if (!args.values.contains("--mcp-config") || args.mcp_servers.empty())
            throw std::runtime_error("mcp-tools requires --mcp-config and at least one --allow-mcp server ID");
        const McpPlan plan(utf8_path(args.get("--mcp-config")),workspace,args.mcp_servers,sandbox);
        resolve_mcp_credentials(plan,cancelled);
        auto extensions=plan.connect(pinned_mcp_credential,cancelled);
        validate_mcp_schema_secrets(extensions);
        Json schemas=Json::array();
        for (const auto& extension : extensions) for (const auto& schema : extension->schemas()) schemas.push_back(schema);
        std::set<std::string> transports;
        const auto document=Json::parse(plan.snapshot());
        for (const auto& server : document.at("servers"))
            if (std::ranges::find(args.mcp_servers,server.at("id").get<std::string>())!=args.mcp_servers.end())
                transports.insert(server.value("transport",std::string("stdio")));
        return {application_redact({{"transport",transports.size()==1 ? *transports.begin() : "mixed"},
                      {"transports",transports},{"servers",args.mcp_servers},{"tools",schemas},
                      {"os_sandbox",sandbox!=SandboxMode::none},{"sandbox",sandbox_mode_name(sandbox)}}), 0};
    }
    std::filesystem::path database;
    try {
        database=std::filesystem::absolute(utf8_path(args.get("--state",path_utf8(workspace / ".codestudio" / "state.sqlite3"))));
        database=std::filesystem::weakly_canonical(database);
    } catch (const std::filesystem::filesystem_error&) {
        throw std::runtime_error("State path cannot be opened");
    }
    if (args.command=="build-review") {
        StateReader reader(database);
        return {review_build_proposal(reader.build_proposal(args.task_id,args.get("--action")),workspace),0};
    }
    if(args.command=="history" || args.command=="show") {
        if(!std::filesystem::is_regular_file(database)) throw std::runtime_error("No state database found; run a task first or specify --state");
        Store store(database);
        if(args.command=="history") return {store.list_tasks(), 0};
        auto data=store.task(args.task_id); data["events"]=store.events(args.task_id);
        return {std::move(data), 0};
    }
    std::unique_ptr<RunLock> workspace_lock,lock;
    std::unique_ptr<Store> owned_store;
    if (!inherited_store) {
        workspace_lock=std::make_unique<RunLock>(workspace / ".codestudio" / "workspace");
        lock=std::make_unique<RunLock>(database);
        owned_store=std::make_unique<Store>(database);
    }
    auto& store=inherited_store ? *inherited_store : *owned_store;
    if(args.command=="recover") return {{{"recovered",store.recover()},{"tools_executed",false}}, 0};
    // A previous crashed run needs explicit reconciliation before a new run can
    // mutate the same state/workspace. Never quietly retry pending actions.
    if(store.has_running_tasks())
        throw std::runtime_error("An interrupted run exists; inspect history and run recover before starting another task");
    std::optional<CommandResult> decision;
    if (args.command=="build-approve" || args.command=="build-reject") {
        if (args.command=="build-approve") {
            // Build execution writes its own receipt before optional model
            // continuation. Resolve the source key before authorizing that
            // write; approval and continuation share one pinned snapshot.
            const auto source_checkpoint=store.checkpoint(args.task_id);
            const auto& binding=source_checkpoint.at("binding");
            if (binding.at("workspace")!=path_utf8(workspace))
                throw std::runtime_error("Build proposal belongs to another workspace");
            const auto& source_provider=binding.at("configuration").at("provider");
            const auto kind=source_provider.value("kind","");
            if (kind!="offline-demo" && kind!="isolated-build") {
                const auto reference=saved_credential_reference(source_provider);
                if (args.values.contains("--credential") && args.get("--credential")!=reference)
                    throw std::runtime_error("Approval credential reference differs from the saved binding");
                (void)resolve_operation_credential(reference,cancelled);
            }
            resolve_saved_mcp_credentials(binding.at("configuration"),cancelled);
        }
        decision=decide_build_proposal(store,workspace,database,args.task_id,args.get("--action"),args.get("--digest"),
                                      args.command=="build-approve",cancelled);
        if (!args.has("--continue-task")) return std::move(*decision);
        try {
            const auto skip=[&](const std::string& reason) {
                decision->value["continuation"]={{"status","skipped"},{"reason",reason}};
                if (cancelled.load()) decision->exit_code=130;
                return std::move(*decision);
            };
            if (cancelled.load() || decision->exit_code==130)
                return skip("Cancelled; explicitly resume the source task to continue");
            // Correlate continuation with this exact outstanding decision. Repeated
            // notifications must not advance a completed task or a later proposal.
            const auto source=store.task(args.task_id);
            if (source.at("status")!="waiting_approval")
                return skip("Source task is no longer waiting for this proposal; use explicit resume for recovery");
            const auto checkpoint=store.checkpoint(args.task_id);
            const auto& state=checkpoint.at("state");
            bool matches=false;
            if (state.value("phase","")=="tools" && state.contains("next_call") && state["next_call"].is_number_unsigned() &&
                state.contains("pending_calls") && state["pending_calls"].is_array()) {
                const auto cursor=state["next_call"].get<std::size_t>();
                if (cursor<state["pending_calls"].size()) {
                    const auto& call=state["pending_calls"][cursor];
                    for (const auto& action : source.at("actions"))
                        if (action.at("id")==args.get("--action") && action.at("call_id")==call.value("id","") &&
                            call.value("name","")=="propose_build") matches=true;
                }
            }
            if (!matches) return skip("Source task is not waiting for this proposal");
            args.command="resume";
            args.values.erase("--action"); args.values.erase("--digest"); args.flags.erase("--continue-task");
        } catch (const std::exception& error) {
            decision->value["continuation"]={{"status","failed"},{"error",error.what()}};
            decision->value=application_redact(std::move(decision->value));
            decision->exit_code=cancelled.load() ? 130 : 2;
            return std::move(*decision);
        }
    }
    // The shared runner stays inside the same locks and cancellation scope for
    // ordinary resume and explicit approval continuation; never recurse here.
    const auto run=[&]() -> CommandResult {
        const bool enrolling=args.command=="schedule-resume";
        const bool recurring=args.command=="cron-create";
        const bool resuming=args.command=="resume" || enrolling;
        Json saved;
        Json saved_task;
        Json saved_config=Json::object();
        if (resuming) {
            saved_task=store.task(args.task_id);
            saved=store.checkpoint(args.task_id);
            resume_preflight(saved_task,saved,workspace);
            validate_resume_state(saved.at("state"),saved_task,enrolling);
            saved_config=saved.at("binding").at("configuration");
            if (!saved_config.is_object() || !saved_config.contains("provider") || !saved_config["provider"].is_object())
                throw std::runtime_error("Checkpoint has no supported provider configuration");
        }
        const bool build_provider=args.command=="build" ||
            (resuming && saved_config.at("provider").value("kind","")=="isolated-build");
        const bool demo_provider=args.command=="demo" ||
            (resuming && saved_config.at("provider").value("kind","")=="offline-demo");
        if (build_provider) {
            if (!args.values.contains("--build-config"))
                throw std::runtime_error("Isolated build resume requires the original --build-config");
            for (const auto& [name, _] : args.values)
                if (name!="--workspace" && name!="--state" && name!="--build-config" && name!="--max-turns" &&
                    !(enrolling && (name=="--delay-ms" || name=="--deadline-ms" || name=="--max-attempts" || name=="--retry-delay-ms")))
                    throw std::runtime_error("Isolated build cannot accept provider, process, MCP or sandbox overrides");
            if (!args.flags.empty() || !args.executables.empty() || !args.mcp_servers.empty())
                throw std::runtime_error("Isolated build cannot accept provider, process, MCP or network grants");
        }
        std::string key,credential_reference;
        if (!build_provider && !demo_provider) {
            if (resuming) {
                const auto& previous=saved_config.at("provider");
                credential_reference=saved_credential_reference(previous);
                if (args.values.contains("--credential") && args.get("--credential")!=credential_reference)
                    throw std::runtime_error("Resume credential reference differs from the saved binding");
            } else credential_reference=args.get("--credential","env:CODESTUDIO_API_KEY");
            key=resolve_operation_credential(credential_reference,cancelled);
        }
        std::unique_ptr<McpPlan> mcp_plan;
        std::filesystem::path manifest;
        Json mcp_binding=nullptr;
        if (args.values.contains("--mcp-config")) {
            manifest=std::filesystem::canonical(utf8_path(args.get("--mcp-config")));
            mcp_plan=std::make_unique<McpPlan>(manifest,workspace,args.mcp_servers,sandbox);
            auto grants=args.mcp_servers;
            std::sort(grants.begin(),grants.end());
            mcp_binding={{"path",path_utf8(manifest)},{"manifest",mcp_plan->snapshot()},{"grants",grants}};
            const auto plugins=mcp_plan->plugin_bindings();
            if (!plugins.empty()) mcp_binding["plugins"]=plugins;
        }
        // Reject changed endpoint/reference/grants before resolving a new MCP
        // credential. The complete provider/policy binding is checked below,
        // still before the plan is permitted to launch or connect anything.
        if (resuming && (!saved_config.contains("mcp") || saved_config.at("mcp")!=mcp_binding))
            throw std::runtime_error("Resume MCP configuration or grants differ from the saved binding; repeat the original settings");
        // Preflight may read secrets for redaction, but token rotation is a
        // side effect and must wait for the complete execution authorization.
        if (mcp_plan) remember_mcp_credentials(*mcp_plan,cancelled);
        ToolPolicy policy;
        policy.sandbox=sandbox;
        policy.workspace=workspace;
        policy.allow_write=args.command=="demo" || args.has("--allow-write");
        const int process_timeout=resuming ? saved.at("binding").at("policy").at("process_timeout_ms").get<int>() : 10000;
        policy.process_timeout=std::chrono::milliseconds(args.number("--process-timeout-ms",process_timeout,100,300000));
        policy.protected_paths={workspace / ".codestudio",database,utf8_path(path_utf8(database)+"-wal"),
                                utf8_path(path_utf8(database)+"-shm"),utf8_path(path_utf8(database)+".run.lock"),
                                utf8_path(path_utf8(database)+".scheduler.run.lock"),
                                utf8_path(path_utf8(database)+".supervisor.run.lock")};
        for(const auto& executable:args.executables) {
            const auto path=utf8_path(executable);
            if(!path.is_absolute()) throw std::runtime_error("--allow-process requires an absolute executable path");
            policy.allowed_executables.push_back(std::filesystem::canonical(path));
        }
        const auto validate_saved_secrets=[&] {
            if (resuming && application_redact(saved)!=saved)
                throw std::runtime_error("Resume checkpoint would require secret redaction and cannot be reused");
            if (resuming) for (const auto& action : saved_task.at("actions")) {
                // Feedback created after the checkpoint needs the same exact
                // redaction check before any MCP launch.
                if (action.contains("continuation") && action["continuation"].contains("result") &&
                    application_redact(action["continuation"]["result"])!=action["continuation"]["result"])
                    throw std::runtime_error("Build feedback requires secret redaction and cannot be reused");
            }
        };
        validate_saved_secrets();
        std::unique_ptr<StreamRedactor> stream_output;
        std::unique_ptr<ModelProvider> provider;
        RunOptions options;
        options.secrets=current_credentials->secrets;
        options.max_turns=args.number("--max-turns",resuming ? saved.at("state").at("max_turns").get<int>() : 8,1,100);
        if ((decision || enrolling || !scheduled.is_null()) && options.max_turns!=saved.at("state").at("max_turns").get<int>())
            throw std::runtime_error("Scheduled and approval continuation retain the saved total turn budget; use explicit resume to change it");
        if (!scheduled.is_null()) {
            options.scheduler_job_id=scheduled.at("id").get<std::string>();
            options.scheduler_claim_token=scheduled.at("claim_token").get<std::string>();
        }
        if (resuming) {
            options.resume_task_id=args.task_id;
            options.prompt=saved_task.at("prompt").get<std::string>();
            const auto& state=saved.at("state");
            if (options.max_turns < state.at("max_turns").get<int>())
                throw std::runtime_error("Resume cannot reduce the saved total turn budget");
            const bool pending_tools=state.at("phase")=="tools" && state.at("next_call").get<std::size_t>() < state.at("pending_calls").size();
            if (!pending_tools && options.max_turns <= state.at("turns").get<int>())
                throw std::runtime_error("Resume needs a larger --max-turns total budget");
        }
        options.binding={{"provider",Json::object()},{"mcp",std::move(mcp_binding)}};
        if (build_provider) {
            provider=std::make_unique<IsolatedBuildProvider>();
            if (!resuming) options.prompt="Execute the explicitly granted isolated workspace build once and report its tool receipt.";
            options.binding["provider"]={{"kind","isolated-build"}};
        } else if(demo_provider) {
            provider=make_demo_provider();
            if (!resuming) options.prompt="Create CODESTUDIO_DEMO.txt containing Hello from CodeStudio-Agent! followed by a newline, then read it back and verify it.";
            options.binding["provider"]={{"kind","offline-demo"}};
            for (const auto* key : {"--base-url","--model","--api","--credential","--max-output-tokens","--timeout-ms"})
                if (args.values.contains(key)) throw std::runtime_error("Offline demo cannot accept provider overrides");
            if (args.has("--no-stream") || args.has("--allow-http-loopback"))
                throw std::runtime_error("Offline demo cannot accept provider overrides");
        } else {
            if (!resuming) options.prompt=args.get("--prompt");
            const auto& previous=resuming ? saved_config.at("provider") : Json::object();
            ProviderConfig config;
            config.base_url=args.get("--base-url",resuming ? previous.at("base_url").get<std::string>() : environment("CODESTUDIO_BASE_URL"));
            config.model=args.get("--model",resuming ? previous.at("model").get<std::string>() : environment("CODESTUDIO_MODEL"));
            config.api_key=key;
            config.api=parse_provider_api(args.get("--api",resuming ? previous.at("api").get<std::string>() :
                (environment("CODESTUDIO_API").empty() ? "chat-completions" : environment("CODESTUDIO_API"))));
            config.stream=args.has("--no-stream") ? false : (resuming ? previous.at("stream").get<bool>() : true);
            config.max_output_tokens=args.number("--max-output-tokens",resuming ? previous.at("max_output_tokens").get<int>() : 4096,1,1000000);
            config.on_text_delta=[&](std::string_view delta) { stream_output->append(delta); };
            config.allow_insecure_loopback=args.has("--allow-http-loopback");
            config.timeout=std::chrono::milliseconds(args.number("--timeout-ms",resuming ? previous.at("timeout_ms").get<int>() : 60000,100,300000));
            options.binding["provider"]={{"base_url",config.base_url},{"model",config.model},{"credential",credential_reference},
                {"api",provider_api_name(config.api)},{"stream",config.stream},{"max_output_tokens",config.max_output_tokens},
                {"timeout_ms",config.timeout.count()},{"allow_http_loopback",config.allow_insecure_loopback}};
            provider=make_openai_compatible_provider(std::move(config));
        }
        if (options.prompt.empty() || options.prompt.size()>65536)
            throw std::runtime_error("Prompt must contain 1..65536 bytes");
        std::vector<std::shared_ptr<ToolExtension>> extensions;
        if (mcp_plan) {
            policy.protected_paths.push_back(manifest);
            for (const auto& path : mcp_plan->protected_paths()) policy.protected_paths.push_back(path);
        }
        std::shared_ptr<WorkspaceBuild> build;
        if (args.values.contains("--build-config")) {
            // Protect custom SQLite rollback journals from imported inputs.
            policy.protected_paths.push_back(utf8_path(path_utf8(database)+"-journal"));
            const auto build_manifest=std::filesystem::absolute(utf8_path(args.get("--build-config"))).lexically_normal();
            policy.protected_paths.push_back(build_manifest);
            build=std::make_shared<WorkspaceBuild>(build_manifest,workspace,policy.protected_paths);
            options.binding["build"]=build->binding();
        }
        std::shared_ptr<BuildProposal> proposals;
        if (args.has("--allow-build-proposals")) {
            policy.protected_paths.push_back(utf8_path(path_utf8(database)+"-journal"));
            options.binding["build_proposals"]={{"version",2},{"approval","explicit-digest"}};
        }
        if (resuming) {
            const auto& binding=saved.at("binding");
            if (binding.at("configuration")!=options.binding || binding.at("policy")!=policy_binding(policy) ||
                binding.at("provider")!=provider->name() || binding.at("workspace")!=path_utf8(workspace))
                throw std::runtime_error("Resume configuration or grants differ from the saved binding; repeat the original settings");
        }
        const auto validate_cron_secrets=[&] {
            if (recurring) {
                const Json metadata{{"name",args.get("--name","Scheduled task")},{"expression",args.get("--cron")},
                    {"timezone",args.get("--timezone")},{"prompt",options.prompt},{"configuration",options.binding},
                    {"policy",policy_binding(policy)}};
                if (application_redact(metadata)!=metadata)
                    throw std::runtime_error("Cron template requires credential redaction and cannot be persisted");
            }
        };
        validate_cron_secrets();
        if (enrolling) {
            if (cancelled.load()) throw std::runtime_error("Scheduling cancelled before enrollment");
            const auto now=scheduler_now_ms();
            const auto delay=args.number("--delay-ms",0,0,604800000);
            const auto deadline=args.number("--deadline-ms",86400000,1,604800000);
            if (deadline<=delay) throw std::runtime_error("Schedule deadline must follow its due time");
            const auto attempts=args.number("--max-attempts",8,1,100);
            const auto retry=args.number("--retry-delay-ms",1000,100,60000);
            const auto resume_argv=resume_arguments(args,policy,workspace,database,args.task_id);
            return {store.schedule_resume(args.task_id,resume_argv,saved.at("binding"),
                store.checkpoint_revision(args.task_id),now+delay,now+deadline,attempts,retry),0};
        }
        if (mcp_plan) resolve_mcp_credentials(*mcp_plan,cancelled);
        // Refresh can introduce new secrets. Recheck saved data and construct
        // every execution-time redactor only after those values are known.
        validate_saved_secrets();
        validate_cron_secrets();
        options.secrets=current_credentials->secrets;
        stream_output=std::make_unique<StreamRedactor>(options.secrets,callbacks.on_text);
        if (args.has("--allow-build-proposals"))
            proposals=std::make_shared<BuildProposal>(workspace,policy.protected_paths,options.secrets);
        if (mcp_plan) {
            auto reserved=build ? build->schemas() : Json::array();
            if (proposals) for (const auto& schema : proposals->schemas()) reserved.push_back(schema);
            extensions=mcp_plan->connect(pinned_mcp_credential,cancelled,
                reserved.size(),reserved.empty() ? 0 : reserved.dump().size());
            validate_mcp_schema_secrets(extensions);
        }
        if (build) extensions.push_back(std::move(build));
        if (proposals) extensions.push_back(std::move(proposals));
        const ToolBroker broker(policy,std::move(extensions));
        if (recurring) {
            if (cancelled.load()) throw std::runtime_error("Cron creation cancelled before enrollment");
            const auto checkpoint=prepare_initial_checkpoint(*provider,broker,options);
            return {store.create_cron(args.get("--name","Scheduled task"),args.get("--cron"),args.get("--timezone"),
                options.prompt,provider->name(),resume_arguments(args,policy,workspace,database,"cron-task"),checkpoint,
                args.number("--deadline-ms",86400000,1,604800000),args.number("--max-attempts",8,1,100),
                args.number("--retry-delay-ms",1000,100,60000),args.number("--max-runs",0,0,1000000),scheduler_now_ms()),0};
        }
        Json result;
        try { result=run_agent(store,*provider,broker,options,cancelled); }
        catch (...) { stream_output->finish(); throw; }
        stream_output->finish();
        result["state_database"]=path_utf8(database);
        if (provider->name()=="isolated-build") {
            const auto task=store.task(result.at("task_id").get<std::string>());
            if (!task.at("actions").empty()) result["build"]=task.at("actions").back().at("result");
        }
        const auto exit_code=result.at("status")=="completed" ? 0 : result.at("status")=="waiting_approval" ? 3 :
                             result.at("status")=="cancelled" ? 130 : 1;
        return {std::move(result), exit_code};
    };
    if (!decision) return run();
    try {
        auto continued=run();
        decision->value["continuation"]={{"status","started"},{"result",std::move(continued.value)},
                                          {"exit_code",continued.exit_code}};
        decision->exit_code=continued.exit_code;
    } catch (const std::exception& error) {
        // The independent decision remains durable even when grants or provider
        // preparation prevent its source task from continuing.
        decision->value["continuation"]={{"status","failed"},{"error",error.what()}};
        decision->exit_code=cancelled.load() ? 130 : 2;
    }
    decision->value=application_redact(std::move(decision->value));
    return std::move(*decision);
}

CommandResult execute_command(const std::vector<std::string>& argv,const std::atomic_bool& cancelled,
                              const CommandCallbacks& callbacks) {
    return with_credential_scope([&]() -> CommandResult {
        if (argv.size()>1 && (argv[1].starts_with("plugin-") || argv[1].starts_with("mcp-auth-") ||
                             argv[1]=="sandbox-resources" || argv[1]=="sandbox-reclaim"))
            return extension_command(argv,cancelled,callbacks,[](const std::string& secret) { current_credentials->remember(secret); });
        if (argv.size()>1 && (argv[1].starts_with("scheduler-") ||
            (argv[1].starts_with("cron-") && argv[1]!="cron-create") || argv[1]=="schedule-list" ||
            argv[1]=="schedule-show" || argv[1]=="schedule-cancel"))
            return scheduler_command(argv,cancelled);
        return execute_impl(argv,cancelled,callbacks);
    });
}

Json prepare_scheduler_jobs(const std::filesystem::path& workspace,const std::filesystem::path& database,
    const std::string& owner,std::int64_t generation) {
    RunLock workspace_lock(workspace/".codestudio"/"workspace");
    RunLock database_lock(database);
    Store store(database);
    store.recover_schedules(owner,generation,scheduler_now_ms());
    if (store.has_running_tasks()) return Json::array();
    store.materialize_crons(path_utf8(workspace),owner,generation,scheduler_now_ms());
    return store.pending_schedules();
}

CommandResult execute_scheduled_job(const std::filesystem::path& workspace,const std::filesystem::path& database,
    const std::string& job_id,const std::string& owner,std::int64_t generation,const std::atomic_bool& cancelled) {
    return with_credential_scope([&]() -> CommandResult {
    RunLock workspace_lock(workspace/".codestudio"/"workspace");
    RunLock database_lock(database);
    Store store(database);
    store.recover_schedules(owner,generation,scheduler_now_ms());
    if (store.has_running_tasks()) return {{{"processed",0},{"reason","Unrelated task requires explicit recovery"}},0};
    const auto claim=store.claim_schedule(job_id,owner,generation,scheduler_now_ms());
    if (claim.is_null()) return {{{"processed",0}},0};
    Json outcome;
    try {
        const auto args=claim.at("resume_argv").get<std::vector<std::string>>();
        const auto parsed=parse(args);
        if (parsed.command!="resume" || claim.at("task_id")!=parsed.task_id ||
            parsed.get("--workspace")!=path_utf8(workspace) || parsed.get("--state")!=path_utf8(database))
            throw std::runtime_error("Scheduled request identity differs from its enrollment");
        const auto result=execute_impl(args,cancelled,{},&store,claim);
        outcome={{"result",result.value},{"exit_code",result.exit_code}};
    } catch (const std::exception& error) {
        const auto reason=application_redact(error.what()).get<std::string>();
        const auto current=store.schedule(job_id);
        if (current.at("status")=="claimed")
            store.finish_scheduler_claim(job_id,claim.at("claim_token").get<std::string>(),reason,scheduler_now_ms());
        else if (current.at("status")=="running") store.recover_schedules(owner,generation,scheduler_now_ms());
        outcome={{"error",reason},{"exit_code",2}};
    }
    return {application_redact({{"processed",1},{"job",store.schedule(job_id)},{"outcome",outcome}}),0};
    });
}
} // namespace codestudio
