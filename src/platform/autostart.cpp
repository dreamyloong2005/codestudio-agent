#include "codestudio/autostart.hpp"
#include "codestudio/digest.hpp"
#include "codestudio/store.hpp"

#include <charconv>
#include <iomanip>
#include <new>
#include <sstream>
#include <stdexcept>
#include <system_error>
#ifdef __linux__
#include "autostart_linux.hpp"
#endif

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <sddl.h>
#include <shellapi.h>
#include <taskschd.h>
#endif

namespace codestudio {
namespace {
constexpr auto source = "CodeStudio-Agent current-user scheduler";
constexpr auto principal_id = "CodeStudio-Agent.CurrentUser";
constexpr auto trigger_id = "CodeStudio-Agent.UserLogon";
constexpr auto action_id = "CodeStudio-Agent.Worker";

#ifdef _WIN32
std::wstring wide(const std::string& text) {
    if (text.size() > 65536 || text.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid autostart text length or NUL");
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid autostart UTF-8 text");
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size))
        throw std::runtime_error("Invalid autostart UTF-8 text");
    return result;
}
std::string narrow(std::wstring_view text) {
    if (text.size() > 65536 || text.find(L'\0') != std::wstring_view::npos)
        throw std::runtime_error("Invalid autostart UTF-16 field");
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid autostart UTF-16 text");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr))
        throw std::runtime_error("Invalid autostart UTF-16 text");
    return result;
}
std::string path_key(const std::string& path) {
    auto input = wide(path);
    for (auto& ch : input) if (ch == L'/') ch = L'\\';
    const auto size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!size) throw std::runtime_error("Cannot normalize autostart path identity");
    std::wstring result(size, L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, input.data(), static_cast<int>(input.size()), result.data(), size, nullptr, nullptr, 0))
        throw std::runtime_error("Cannot normalize autostart path identity");
    return narrow(result);
}
std::string quote(const std::string& value) {
    std::string result = "\"";
    std::size_t slashes = 0;
    for (const char ch : value) {
        if (ch == '\\') { ++slashes; continue; }
        result.append(ch == '"' ? slashes * 2 + 1 : slashes, '\\');
        result += ch; slashes = 0;
    }
    result.append(slashes * 2, '\\'); result += '"'; return result;
}
Json arguments(const std::string& text) {
    // CommandLineToArgvW is a parser, not a shell or a process launcher.
    const auto input = L"codestudio-agent " + wide(text);
    int count = 0;
    auto* argv = CommandLineToArgvW(input.c_str(), &count);
    if (!argv) throw std::runtime_error("Cannot parse autostart command line");
    Json result = Json::array();
    try { for (int i = 1; i < count; ++i) result.push_back(narrow(argv[i])); }
    catch (...) { LocalFree(argv); throw; }
    LocalFree(argv); return result;
}

class ComError : public std::runtime_error {
public:
    HRESULT code;
    ComError(const char* operation, HRESULT value) : std::runtime_error(message(operation, value)), code(value) {}
private:
    static std::string message(const char* operation, HRESULT value) {
        std::ostringstream out;
        out << operation << " failed (HRESULT 0x" << std::hex << std::setfill('0') << std::setw(8)
            << static_cast<unsigned long>(value) << ')';
        return out.str();
    }
};
void checked(HRESULT result, const char* operation) { if (FAILED(result)) throw ComError(operation, result); }
bool unavailable(HRESULT value) {
    return value == REGDB_E_CLASSNOTREG || value == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
        value == HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE) || value == SCHED_E_SERVICE_NOT_RUNNING;
}
bool missing(HRESULT value) {
    return value == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || value == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}
class Apartment {
    bool initialized_ = false;
public:
    Apartment() {
        const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == RPC_E_CHANGED_MODE) return;
        checked(result, "CoInitializeEx(autostart)"); initialized_ = true;
    }
    ~Apartment() { if (initialized_) CoUninitialize(); }
};
template<class T> class Com {
    T* value_ = nullptr;
public:
    Com() = default;
    ~Com() { if (value_) value_->Release(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* operator->() const { return value_; }
    T* get() const { return value_; }
    T** out() { if (value_) throw std::logic_error("COM pointer already populated"); return &value_; }
};
class Bstr {
    BSTR value_ = nullptr;
public:
    Bstr() = default;
    explicit Bstr(const std::string& value) {
        const auto text = wide(value);
        value_ = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        if (!value_) throw std::bad_alloc();
    }
    ~Bstr() { SysFreeString(value_); }
    Bstr(const Bstr&) = delete;
    BSTR get() const { return value_; }
    BSTR* out() { return &value_; }
    std::string text() const { return value_ ? narrow(std::wstring_view(value_, SysStringLen(value_))) : ""; }
};
template<class F> std::string text_property(F get) {
    Bstr value; checked(get(value.out()), "Read autostart text property"); return value.text();
}
template<class F> bool boolean_property(F get) {
    VARIANT_BOOL value = VARIANT_FALSE; checked(get(&value), "Read autostart boolean property"); return value != VARIANT_FALSE;
}
template<class T, class F> int enum_property(F get) {
    T value{}; checked(get(&value), "Read autostart enum property"); return static_cast<int>(value);
}
std::string token_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::system_error(GetLastError(), std::system_category(), "OpenProcessToken(autostart)");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    if (!size || !GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) {
        const auto error = GetLastError(); CloseHandle(token);
        throw std::system_error(error, std::system_category(), "GetTokenInformation(autostart)");
    }
    CloseHandle(token);
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text))
        throw std::system_error(GetLastError(), std::system_category(), "ConvertSidToStringSidW(autostart)");
    try { auto result = narrow(text); LocalFree(text); return result; }
    catch (...) { LocalFree(text); throw; }
}
std::filesystem::path executable_path() {
    std::vector<wchar_t> path(512);
    for (;;) {
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size) throw std::system_error(GetLastError(), std::system_category(), "GetModuleFileNameW(autostart)");
        if (size < path.size()) return std::filesystem::canonical(std::filesystem::path(std::wstring(path.data(), size)));
        if (path.size() >= 32768) throw std::runtime_error("Autostart executable path exceeds its limit");
        path.resize(path.size() * 2);
    }
}
std::string account_sid(const std::string& account) {
    if (account.empty()) return {};
    PSID parsed = nullptr;
    if (ConvertStringSidToSidW(wide(account).c_str(), &parsed)) { LocalFree(parsed); return account; }
    DWORD sid_size = 0, domain_size = 0; SID_NAME_USE type{};
    LookupAccountNameW(nullptr, wide(account).c_str(), nullptr, &sid_size, nullptr, &domain_size, &type);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
    std::vector<unsigned char> sid(sid_size); std::vector<wchar_t> domain(domain_size);
    if (!LookupAccountNameW(nullptr, wide(account).c_str(), sid.data(), &sid_size, domain.data(), &domain_size, &type)) return {};
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid.data(), &text)) return {};
    try { auto result = narrow(text); LocalFree(text); return result; }
    catch (...) { LocalFree(text); throw; }
}
bool zero_duration(const std::string& value) {
    return value.empty() || value == "PT0S" || value == "PT0M" || value == "PT0H" || value == "P0D";
}
Json inspect(ITaskDefinition* definition) {
    Json result;
    Com<IRegistrationInfo> registration; checked(definition->get_RegistrationInfo(registration.out()), "Read task registration");
    result["source"] = text_property([&](BSTR* out) { return registration->get_Source(out); });
    result["uri"] = text_property([&](BSTR* out) { return registration->get_URI(out); });
    Com<IPrincipal> principal; checked(definition->get_Principal(principal.out()), "Read task principal");
    result["principal_id"] = text_property([&](BSTR* out) { return principal->get_Id(out); });
    result["user_sid"] = account_sid(text_property([&](BSTR* out) { return principal->get_UserId(out); }));
    result["group_id"] = text_property([&](BSTR* out) { return principal->get_GroupId(out); });
    result["logon_type"] = enum_property<TASK_LOGON_TYPE>([&](TASK_LOGON_TYPE* out) { return principal->get_LogonType(out); });
    result["run_level"] = enum_property<TASK_RUNLEVEL_TYPE>([&](TASK_RUNLEVEL_TYPE* out) { return principal->get_RunLevel(out); });
    Com<ITriggerCollection> triggers; checked(definition->get_Triggers(triggers.out()), "Read task triggers");
    LONG count = 0; checked(triggers->get_Count(&count), "Read task trigger count"); result["trigger_count"] = count;
    if (count == 1) {
        Com<ITrigger> trigger; checked(triggers->get_Item(1, trigger.out()), "Read task trigger");
        const auto type = enum_property<TASK_TRIGGER_TYPE2>([&](TASK_TRIGGER_TYPE2* out) { return trigger->get_Type(out); });
        result["trigger_type"] = type;
        result["trigger_id"] = text_property([&](BSTR* out) { return trigger->get_Id(out); });
        result["trigger_enabled"] = boolean_property([&](VARIANT_BOOL* out) { return trigger->get_Enabled(out); });
        result["start_boundary"] = text_property([&](BSTR* out) { return trigger->get_StartBoundary(out); });
        result["end_boundary"] = text_property([&](BSTR* out) { return trigger->get_EndBoundary(out); });
        result["unlimited_trigger_duration"] = zero_duration(text_property([&](BSTR* out) { return trigger->get_ExecutionTimeLimit(out); }));
        Com<IRepetitionPattern> repeat; checked(trigger->get_Repetition(repeat.out()), "Read task trigger repetition");
        result["repetition_interval"] = text_property([&](BSTR* out) { return repeat->get_Interval(out); });
        result["repetition_duration"] = text_property([&](BSTR* out) { return repeat->get_Duration(out); });
        if (type == TASK_TRIGGER_LOGON) {
            Com<ILogonTrigger> logon; checked(trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logon.out())), "Read logon trigger");
            result["trigger_user_sid"] = account_sid(text_property([&](BSTR* out) { return logon->get_UserId(out); }));
            result["zero_delay"] = zero_duration(text_property([&](BSTR* out) { return logon->get_Delay(out); }));
        }
    }
    Com<IActionCollection> actions; checked(definition->get_Actions(actions.out()), "Read task actions");
    checked(actions->get_Count(&count), "Read task action count"); result["action_count"] = count;
    result["action_context"] = text_property([&](BSTR* out) { return actions->get_Context(out); });
    if (count == 1) {
        Com<IAction> action; checked(actions->get_Item(1, action.out()), "Read task action");
        const auto type = enum_property<TASK_ACTION_TYPE>([&](TASK_ACTION_TYPE* out) { return action->get_Type(out); });
        result["action_type"] = type;
        result["action_id"] = text_property([&](BSTR* out) { return action->get_Id(out); });
        if (type == TASK_ACTION_EXEC) {
            Com<IExecAction> exec; checked(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.out())), "Read executable action");
            result["executable"] = text_property([&](BSTR* out) { return exec->get_Path(out); });
            result["arguments"] = text_property([&](BSTR* out) { return exec->get_Arguments(out); });
            result["working_directory"] = text_property([&](BSTR* out) { return exec->get_WorkingDirectory(out); });
        }
    }
    Com<ITaskSettings> settings; checked(definition->get_Settings(settings.out()), "Read task settings");
    result["multiple_instances"] = enum_property<TASK_INSTANCES_POLICY>([&](TASK_INSTANCES_POLICY* out) { return settings->get_MultipleInstances(out); });
    result["hidden"] = boolean_property([&](VARIANT_BOOL* out) { return settings->get_Hidden(out); });
    result["allow_demand_start"] = boolean_property([&](VARIANT_BOOL* out) { return settings->get_AllowDemandStart(out); });
    result["disallow_battery"] = boolean_property([&](VARIANT_BOOL* out) { return settings->get_DisallowStartIfOnBatteries(out); });
    result["stop_on_battery"] = boolean_property([&](VARIANT_BOOL* out) { return settings->get_StopIfGoingOnBatteries(out); });
    result["run_only_idle"] = boolean_property([&](VARIANT_BOOL* out) { return settings->get_RunOnlyIfIdle(out); });
    result["run_only_network"] = boolean_property([&](VARIANT_BOOL* out) { return settings->get_RunOnlyIfNetworkAvailable(out); });
    result["wake_to_run"] = boolean_property([&](VARIANT_BOOL* out) { return settings->get_WakeToRun(out); });
    result["start_when_available"] = boolean_property([&](VARIANT_BOOL* out) { return settings->get_StartWhenAvailable(out); });
    result["unlimited_duration"] = zero_duration(text_property([&](BSTR* out) { return settings->get_ExecutionTimeLimit(out); }));
    int restart_count = 0; checked(settings->get_RestartCount(&restart_count), "Read task restart count"); result["restart_count"] = restart_count;
    return result;
}
void configure(ITaskDefinition* definition, const Json& plan) {
    const Bstr sid(plan.at("user_sid")), executable(plan.at("executable")), directory(plan.at("workspace"));
    Com<IRegistrationInfo> registration; checked(definition->get_RegistrationInfo(registration.out()), "Get registration info");
    checked(registration->put_Source(Bstr(source).get()), "Set task source");
    checked(registration->put_URI(Bstr(plan.at("uri")).get()), "Set task identity");
    checked(registration->put_Description(Bstr("Starts the explicitly registered CodeStudio-Agent workspace scheduler when its user logs on.").get()), "Set task description");
    Com<IPrincipal> principal; checked(definition->get_Principal(principal.out()), "Get task principal");
    checked(principal->put_Id(Bstr(principal_id).get()), "Set principal identity");
    checked(principal->put_UserId(sid.get()), "Set current user");
    checked(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN), "Set interactive token");
    checked(principal->put_RunLevel(TASK_RUNLEVEL_LUA), "Set least privilege");
    Com<ITriggerCollection> triggers; checked(definition->get_Triggers(triggers.out()), "Get task triggers");
    Com<ITrigger> trigger; checked(triggers->Create(TASK_TRIGGER_LOGON, trigger.out()), "Create logon trigger");
    checked(trigger->put_Id(Bstr(trigger_id).get()), "Set logon trigger identity");
    checked(trigger->put_Enabled(VARIANT_TRUE), "Enable logon trigger");
    checked(trigger->put_ExecutionTimeLimit(Bstr("PT0S").get()), "Remove logon trigger duration limit");
    Com<ILogonTrigger> logon; checked(trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logon.out())), "Get logon trigger");
    checked(logon->put_UserId(sid.get()), "Bind logon trigger to current user");
    checked(logon->put_Delay(Bstr("PT0S").get()), "Set logon delay");
    Com<IActionCollection> actions; checked(definition->get_Actions(actions.out()), "Get task actions");
    checked(actions->put_Context(Bstr(principal_id).get()), "Set action principal");
    Com<IAction> action; checked(actions->Create(TASK_ACTION_EXEC, action.out()), "Create worker action");
    checked(action->put_Id(Bstr(action_id).get()), "Set worker action identity");
    Com<IExecAction> exec; checked(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.out())), "Get worker executable action");
    checked(exec->put_Path(executable.get()), "Set worker executable");
    checked(exec->put_Arguments(Bstr(plan.at("arguments")).get()), "Set fixed worker arguments");
    checked(exec->put_WorkingDirectory(directory.get()), "Set worker directory");
    Com<ITaskSettings> settings; checked(definition->get_Settings(settings.out()), "Get task settings");
    checked(settings->put_Enabled(VARIANT_TRUE), "Enable task");
    checked(settings->put_Hidden(VARIANT_TRUE), "Hide task");
    checked(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW), "Prevent duplicate task instances");
    checked(settings->put_AllowDemandStart(VARIANT_FALSE), "Disable manual task execution");
    checked(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE), "Allow battery startup");
    checked(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE), "Preserve battery execution");
    checked(settings->put_RunOnlyIfIdle(VARIANT_FALSE), "Disable idle condition");
    checked(settings->put_RunOnlyIfNetworkAvailable(VARIANT_FALSE), "Disable network condition");
    checked(settings->put_WakeToRun(VARIANT_FALSE), "Disable wake condition");
    checked(settings->put_StartWhenAvailable(VARIANT_TRUE), "Enable missed logon startup");
    checked(settings->put_ExecutionTimeLimit(Bstr("PT0S").get()), "Remove default worker duration limit");
    checked(settings->put_RestartCount(0), "Disable implicit restart");
}
int numeric_argument(const Json& argument) {
    const auto value = argument.get<std::string>(); int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::runtime_error("Invalid registered scheduler numeric argument");
    return result;
}
AutostartOptions observed_options(const Json& argv) {
    if ((argv.size() != 7 && argv.size() != 11) || argv[1] != "--workspace" ||
        argv[3] != "--state" || argv[5] != "--poll-ms")
        throw std::runtime_error("Invalid registered scheduler command");
    AutostartOptions options;
    options.poll_ms = numeric_argument(argv.at(6));
    if (argv.size() == 7) {
        if (argv[0] != "scheduler-run") throw std::runtime_error("Invalid registered worker command");
    } else {
        if (argv[0] != "scheduler-supervise" || argv[7] != "--max-restarts" || argv[9] != "--restart-delay-ms")
            throw std::runtime_error("Invalid registered supervisor command");
        options.supervise = true;
        options.max_restarts = numeric_argument(argv.at(8));
        options.restart_delay_ms = numeric_argument(argv.at(10));
    }
    validate_autostart_options(options);
    return options;
}
void set_reported_options(Json& result, const AutostartOptions& options) {
    result["poll_ms"] = options.poll_ms;
    result["supervised"] = options.supervise;
    if (options.supervise) {
        result["max_restarts"] = options.max_restarts;
        result["restart_delay_ms"] = options.restart_delay_ms;
    } else {
        result.erase("max_restarts");
        result.erase("restart_delay_ms");
    }
}
#endif
}

namespace autostart_detail {
Json plan(const std::filesystem::path& executable, const std::filesystem::path& workspace,
          const std::filesystem::path& database, const std::string& sid, const AutostartOptions& options) {
#ifdef _WIN32
    validate_autostart_options(options);
    if (sid.empty() || sid.size() > 256) throw std::invalid_argument("Invalid autostart user SID");
    PSID parsed = nullptr;
    if (!ConvertStringSidToSidW(wide(sid).c_str(), &parsed)) throw std::invalid_argument("Invalid autostart user SID");
    const bool valid = IsValidSid(parsed) != FALSE; LocalFree(parsed);
    if (!valid) throw std::invalid_argument("Invalid autostart user SID");
    Json paths = Json::array();
    for (const auto* path : {&executable, &workspace, &database}) {
        if (!path->is_absolute()) throw std::invalid_argument("Autostart paths must be absolute");
        const auto text = path_utf8(path->lexically_normal());
        if (text.empty() || text.find('%') != std::string::npos)
            throw std::invalid_argument("Autostart paths cannot contain Task Scheduler environment expansions");
        (void)wide(text); paths.push_back(text);
    }
    auto command = std::string(options.supervise ? "scheduler-supervise --workspace " : "scheduler-run --workspace ") + quote(paths[1]) +
        " --state " + quote(paths[2]) + " --poll-ms " + std::to_string(options.poll_ms);
    if (options.supervise)
        command += " --max-restarts " + std::to_string(options.max_restarts) +
            " --restart-delay-ms " + std::to_string(options.restart_delay_ms);
    if (wide(command).size() + wide(paths[0]).size() + 4 >= 32767)
        throw std::invalid_argument("Autostart command line exceeds Windows limit");
    const auto identity = sha256_bytes(Json::array({sid, path_key(paths[1]), path_key(paths[2])}).dump());
    const auto task_name = "CodeStudio-Agent-" + identity;
    // Task Scheduler persists URI as the absolute registration path, even if
    // the supplied definition used a custom URI. Use that native identity from
    // the outset so the same strict contract applies before and after saving.
    Json result{{"task_name",task_name},{"uri","\\" + task_name},
        {"source",source},{"user_sid",sid},{"workspace",paths[1]},{"state_database",paths[2]},
        {"executable",paths[0]},{"working_directory",paths[1]},{"arguments",command},
        {"principal_id",principal_id},{"group_id",""},{"logon_type",TASK_LOGON_INTERACTIVE_TOKEN},{"run_level",TASK_RUNLEVEL_LUA},
        {"trigger_count",1},{"trigger_type",TASK_TRIGGER_LOGON},{"trigger_id",trigger_id},{"trigger_enabled",true},
        {"trigger_user_sid",sid},{"start_boundary",""},{"end_boundary",""},{"unlimited_trigger_duration",true},{"repetition_interval",""},
        {"repetition_duration",""},{"zero_delay",true},{"action_count",1},{"action_type",TASK_ACTION_EXEC},
        {"action_id",action_id},{"action_context",principal_id},{"multiple_instances",TASK_INSTANCES_IGNORE_NEW},
        {"hidden",true},{"allow_demand_start",false},{"disallow_battery",false},{"stop_on_battery",false},
        {"run_only_idle",false},{"run_only_network",false},{"wake_to_run",false},{"start_when_available",true},
        {"unlimited_duration",true},{"restart_count",0}};
    set_reported_options(result, options);
    return result;
#elif defined(__linux__)
    return linux_autostart_plan(executable,workspace,database,sid,options);
#else
    (void)executable; (void)workspace; (void)database; (void)sid; (void)options;
    throw std::runtime_error("Login autostart is unsupported on this platform");
#endif
}
bool matches(const Json& expected, const Json& observed, bool check_options) {
#ifdef _WIN32
    try {
        for (const auto& [key, value] : expected.items()) {
            if (key == "task_name" || key == "workspace" || key == "state_database" || key == "poll_ms" ||
                key == "supervised" || key == "max_restarts" || key == "restart_delay_ms") continue;
            if (!observed.contains(key)) return false;
            if (key == "arguments") {
                auto left = arguments(value), right = arguments(observed.at(key));
                (void)observed_options(left);
                (void)observed_options(right);
                if (path_key(left[2]) != path_key(right[2]) || path_key(left[4]) != path_key(right[4])) return false;
                if (check_options) {
                    if (left.size() != right.size()) return false;
                    for (std::size_t index = 0; index < left.size(); ++index)
                        if (index != 2 && index != 4 && left[index] != right[index]) return false;
                }
            } else if (key == "executable" || key == "working_directory") {
                if (path_key(value) != path_key(observed.at(key))) return false;
            } else if (value != observed.at(key)) return false;
        }
        return true;
    } catch (const std::exception&) { return false; }
#elif defined(__linux__)
    return linux_autostart_matches(expected,observed,check_options);
#else
    (void)expected; (void)observed; (void)check_options; return false;
#endif
}
}

Json scheduler_autostart(const std::filesystem::path& workspace, const std::filesystem::path& database,
                         std::string_view action, const AutostartOptions& options) {
    if (action != "status" && action != "enable" && action != "disable")
        throw std::invalid_argument("Autostart action must be status, enable or disable");
#ifdef _WIN32
    const auto directory = std::filesystem::canonical(workspace);
    if (!std::filesystem::is_directory(directory)) throw std::invalid_argument("Autostart workspace must be a directory");
    const auto state = std::filesystem::weakly_canonical(std::filesystem::absolute(database));
    const auto plan = autostart_detail::plan(executable_path(), directory, state, token_sid(), options);
    Json result{{"available",true},{"registered",false},{"enabled",false},{"conflict",false},
        {"task_name",plan.at("task_name")},{"workspace",plan.at("workspace")},{"state_database",plan.at("state_database")},
        {"executable",plan.at("executable")},{"user_sid",plan.at("user_sid")},
        {"trigger","current_user_logon"},{"started",false}};
    set_reported_options(result, options);
    try {
        Apartment apartment;
        Com<ITaskService> service;
        checked(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                 reinterpret_cast<void**>(service.out())), "Create Windows Task Scheduler");
        VARIANT empty{}; VariantInit(&empty);
        checked(service->Connect(empty, empty, empty, empty), "Connect Windows Task Scheduler");
        Com<ITaskFolder> folder; checked(service->GetFolder(Bstr("\\").get(), folder.out()), "Open Task Scheduler root");
        const Bstr name(plan.at("task_name"));
        Com<IRegisteredTask> existing;
        const auto found = folder->GetTask(name.get(), existing.out());
        if (FAILED(found) && !missing(found)) checked(found, "Read registered autostart task");
        if (SUCCEEDED(found)) {
            result["registered"] = true;
            result["enabled"] = boolean_property([&](VARIANT_BOOL* out) { return existing->get_Enabled(out); });
            Com<ITaskDefinition> definition; checked(existing->get_Definition(definition.out()), "Read registered autostart definition");
            const auto observed = inspect(definition.get());
            const bool owned = autostart_detail::matches(plan, observed, false);
            result["conflict"] = !owned;
            if (owned) set_reported_options(result, observed_options(arguments(observed.at("arguments"))));
            if (action == "status") return result;
            if (!owned || (action == "enable" && !autostart_detail::matches(plan, observed)))
                throw std::runtime_error("Autostart registration conflicts with this executable or configuration; no task was changed");
            if (action == "disable") {
                // No PID is opened or killed. Removing registration only stops
                // future logon launches; scheduler-stop controls a live worker.
                checked(folder->DeleteTask(name.get(), 0), "Remove owned autostart task");
                result["registered"] = false; result["enabled"] = false; result["removed"] = true;
                return result;
            }
            if (!result.at("enabled").get<bool>()) checked(existing->put_Enabled(VARIANT_TRUE), "Enable owned autostart task");
            result["enabled"] = boolean_property([&](VARIANT_BOOL* out) { return existing->get_Enabled(out); });
            if (!result.at("enabled").get<bool>()) throw std::runtime_error("Owned autostart task did not become enabled");
            result["created"] = false; return result;
        }
        if (action != "enable") { if (action == "disable") result["removed"] = false; return result; }
        Com<ITaskDefinition> definition; checked(service->NewTask(0, definition.out()), "Create logon task definition");
        configure(definition.get(), plan);
        if (!autostart_detail::matches(plan, inspect(definition.get())))
            throw std::runtime_error("Autostart definition did not preserve the required execution contract");
        Bstr sid(plan.at("user_sid")); VARIANT user{}; user.vt = VT_BSTR; user.bstrVal = sid.get();
        Com<IRegisteredTask> registered;
        checked(folder->RegisterTaskDefinition(name.get(), definition.get(), TASK_CREATE | TASK_IGNORE_REGISTRATION_TRIGGERS,
            user, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, registered.out()), "Register current-user logon task");
        // TASK_CREATE, not CREATE_OR_UPDATE: a competing or foreign registration
        // can never be overwritten between the read and the create operation.
        Com<ITaskDefinition> saved; checked(registered->get_Definition(saved.out()), "Read back autostart definition");
        if (!autostart_detail::matches(plan, inspect(saved.get())))
            throw std::runtime_error("Registered autostart task differs from the requested execution contract; inspect its status");
        result["registered"] = true;
        result["enabled"] = boolean_property([&](VARIANT_BOOL* out) { return registered->get_Enabled(out); });
        if (!result.at("enabled").get<bool>()) throw std::runtime_error("Registered autostart task is not enabled");
        result["created"] = true; return result;
    } catch (const ComError& error) {
        if (action == "status" && unavailable(error.code)) {
            result["available"] = false; result["reason"] = error.what(); return result;
        }
        throw;
    }
#elif defined(__linux__)
    return linux_autostart(workspace,database,action,options);
#else
    (void)workspace; (void)database; (void)options;
    throw std::runtime_error("Login autostart is unsupported on this platform");
#endif
}
}
