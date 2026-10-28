#include "autostart_macos.hpp"

#ifdef __APPLE__
#include "codestudio/digest.hpp"
#include "codestudio/store.hpp"
#include "codestudio/types.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <stdio.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace codestudio {
namespace {
namespace fs=std::filesystem;
class Fd {
    int value_=-1;
public:
    explicit Fd(int value=-1):value_(value) {}
    ~Fd() { if (value_>=0) close(value_); }
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other) noexcept:value_(other.value_) { other.value_=-1; }
    int get() const { return value_; }
};
[[noreturn]] void fail(const char* operation) { throw std::system_error(errno,std::generic_category(),operation); }
std::string xml(const std::string& value) {
    if (value.empty() || value.size()>32768) throw std::invalid_argument("Invalid LaunchAgent argument length");
    const auto string=CFStringCreateWithBytes(kCFAllocatorDefault,reinterpret_cast<const UInt8*>(value.data()),
        static_cast<CFIndex>(value.size()),kCFStringEncodingUTF8,false);
    if (!string) throw std::invalid_argument("LaunchAgent arguments must be valid UTF-8");
    CFRelease(string);
    std::string result;
    for (const unsigned char ch:value) {
        if (ch<32 || ch==127) throw std::invalid_argument("LaunchAgent arguments cannot contain control bytes");
        switch (ch) {
            case '&': result+="&amp;"; break;
            case '<': result+="&lt;"; break;
            case '>': result+="&gt;"; break;
            case '"': result+="&quot;"; break;
            case '\'': result+="&apos;"; break;
            default: result+=static_cast<char>(ch);
        }
    }
    return result;
}
int number(std::string_view value) {
    int result=0;
    const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),result);
    if (error!=std::errc{} || end!=value.data()+value.size() || std::to_string(result)!=value)
        throw std::invalid_argument("Invalid LaunchAgent integer");
    return result;
}
void describe_options(Json& result,const AutostartOptions& options) {
    result["poll_ms"]=options.poll_ms; result["supervised"]=options.supervise;
    if (options.supervise) {
        result["max_restarts"]=options.max_restarts; result["restart_delay_ms"]=options.restart_delay_ms;
    } else { result.erase("max_restarts"); result.erase("restart_delay_ms"); }
}
AutostartOptions json_options(const Json& plan) {
    const auto integer=[&](const char* key) {
        const auto& value=plan.at(key);
        if (!value.is_number_integer() || value<0 || value>60000) throw std::invalid_argument("Invalid autostart integer metadata");
        return value.get<int>();
    };
    AutostartOptions options;
    options.poll_ms=integer("poll_ms"); options.supervise=plan.at("supervised").get<bool>();
    if (options.supervise) {
        options.max_restarts=integer("max_restarts"); options.restart_delay_ms=integer("restart_delay_ms");
    } else if (plan.contains("max_restarts") || plan.contains("restart_delay_ms"))
        throw std::invalid_argument("Unsupervised LaunchAgent has restart metadata");
    validate_autostart_options(options); return options;
}
AutostartOptions plist_options(const std::string& text) {
    const auto data=CFDataCreate(kCFAllocatorDefault,reinterpret_cast<const UInt8*>(text.data()),static_cast<CFIndex>(text.size()));
    if (!data) throw std::runtime_error("Cannot read LaunchAgent plist");
    const auto plist=CFPropertyListCreateWithData(kCFAllocatorDefault,data,kCFPropertyListImmutable,nullptr,nullptr);
    CFRelease(data);
    if (!plist) throw std::runtime_error("Malformed LaunchAgent plist");
    try {
        if (CFGetTypeID(plist)!=CFDictionaryGetTypeID()) throw std::runtime_error("Malformed LaunchAgent dictionary");
        const auto raw=CFDictionaryGetValue(static_cast<CFDictionaryRef>(plist),CFSTR("ProgramArguments"));
        if (!raw || CFGetTypeID(raw)!=CFArrayGetTypeID()) throw std::runtime_error("Malformed LaunchAgent arguments");
        const auto args=static_cast<CFArrayRef>(raw);
        const auto count=CFArrayGetCount(args);
        if (count!=8 && count!=12) throw std::runtime_error("Unexpected LaunchAgent arguments");
        const auto string=[&](CFIndex index) {
            const auto value=CFArrayGetValueAtIndex(args,index);
            if (!value || CFGetTypeID(value)!=CFStringGetTypeID()) throw std::runtime_error("Malformed LaunchAgent string");
            char buffer[32769];
            if (!CFStringGetCString(static_cast<CFStringRef>(value),buffer,sizeof buffer,kCFStringEncodingUTF8))
                throw std::runtime_error("Invalid LaunchAgent string");
            return std::string(buffer);
        };
        AutostartOptions options;
        const auto command=string(1);
        if (command!="scheduler-run" && command!="scheduler-supervise") throw std::runtime_error("Foreign LaunchAgent command");
        options.supervise=command=="scheduler-supervise";
        if (count!=(options.supervise?12:8) || string(2)!="--workspace" || string(4)!="--state" || string(6)!="--poll-ms")
            throw std::runtime_error("Unexpected LaunchAgent argument contract");
        options.poll_ms=number(string(7));
        if (options.supervise) {
            if (string(8)!="--max-restarts" || string(10)!="--restart-delay-ms") throw std::runtime_error("Unexpected LaunchAgent restart flags");
            options.max_restarts=number(string(9)); options.restart_delay_ms=number(string(11));
        }
        validate_autostart_options(options); CFRelease(plist); return options;
    } catch (...) { CFRelease(plist); throw; }
}
fs::path executable_path() {
    uint32_t size=0; (void)_NSGetExecutablePath(nullptr,&size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(),&size)!=0) throw std::runtime_error("Cannot resolve LaunchAgent executable");
    return fs::canonical(buffer.data());
}
fs::path home_directory() {
    const auto* entry=getpwuid(geteuid());
    if (!entry || !entry->pw_dir || !*entry->pw_dir) throw std::runtime_error("Cannot resolve current-user home directory");
    const fs::path home(entry->pw_dir);
    if (!home.is_absolute() || home.lexically_normal()!=home) throw std::runtime_error("Unsafe current-user home directory");
    return home;
}
struct Directory {
    std::vector<Fd> chain;
    std::vector<std::string> names;
    int get() const { return chain.empty()?-1:chain.back().get(); }
    void verify() const {
        for (std::size_t index=1;index<chain.size();++index) {
            struct stat opened{},entry{};
            if (fstat(chain[index].get(),&opened)!=0 || fstatat(chain[index-1].get(),names[index-1].c_str(),&entry,AT_SYMLINK_NOFOLLOW)!=0 ||
                opened.st_dev!=entry.st_dev || opened.st_ino!=entry.st_ino || !S_ISDIR(entry.st_mode) || (entry.st_mode&0022) ||
                (entry.st_uid!=0 && entry.st_uid!=geteuid()))
                throw std::runtime_error("LaunchAgent directory ownership or identity changed during control");
        }
    }
};
Directory open_directory(const fs::path& path,bool create) {
    Directory result;
    result.chain.emplace_back(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    if (result.get()<0) fail("Open LaunchAgent filesystem root");
    for (const auto& component:path.relative_path()) {
        const auto name=component.string();
        if (name.empty() || name=="." || name=="..") throw std::runtime_error("Unsafe LaunchAgent directory component");
        auto next=openat(result.get(),name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (next<0 && errno==ENOENT && create) {
            if (mkdirat(result.get(),name.c_str(),0700)!=0 && errno!=EEXIST) fail("Create LaunchAgent directory");
            next=openat(result.get(),name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        }
        if (next<0) {
            if (errno==ENOENT && !create) return {};
            fail("Open anchored LaunchAgent directory");
        }
        result.names.push_back(name); result.chain.emplace_back(next); result.verify();
    }
    struct stat info{};
    if (fstat(result.get(),&info)!=0) fail("Inspect LaunchAgent directory");
    if (info.st_uid!=geteuid()) throw std::runtime_error("LaunchAgent directory must be owned by the current user");
    return result;
}
struct Registration { bool exists=false; std::string text; struct stat info{}; };
void same_entry(int directory,const std::string& name,const struct stat& original) {
    struct stat info{};
    if (fstatat(directory,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)!=0 || info.st_dev!=original.st_dev || info.st_ino!=original.st_ino ||
        !S_ISREG(info.st_mode) || info.st_uid!=geteuid() || info.st_nlink!=1 || (info.st_mode&0022) || info.st_size!=original.st_size ||
        info.st_mtimespec.tv_sec!=original.st_mtimespec.tv_sec || info.st_mtimespec.tv_nsec!=original.st_mtimespec.tv_nsec ||
        info.st_ctimespec.tv_sec!=original.st_ctimespec.tv_sec || info.st_ctimespec.tv_nsec!=original.st_ctimespec.tv_nsec)
        throw std::runtime_error("LaunchAgent registration changed during control; no replacement was removed");
}
Registration read_registration(int directory,const std::string& name) {
    if (directory<0) return {};
    Fd file(openat(directory,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    if (file.get()<0) { if (errno==ENOENT) return {}; fail("Open LaunchAgent registration"); }
    Registration result; result.exists=true;
    if (fstat(file.get(),&result.info)!=0) fail("Inspect LaunchAgent registration");
    const auto& info=result.info;
    if (!S_ISREG(info.st_mode) || info.st_uid!=geteuid() || info.st_nlink!=1 || (info.st_mode&0022) || info.st_size<0 || info.st_size>262144)
        throw std::runtime_error("LaunchAgent ownership, type or size conflicts with the expected registration");
    char buffer[4096];
    for (;;) {
        const auto count=read(file.get(),buffer,sizeof buffer);
        if (count<0) { if (errno==EINTR) continue; fail("Read LaunchAgent registration"); }
        if (!count) break;
        if (result.text.size()+count>262144) throw std::runtime_error("LaunchAgent registration exceeds its read limit");
        result.text.append(buffer,static_cast<std::size_t>(count));
    }
    same_entry(directory,name,info); return result;
}
void write_registration(const Directory& directory,const std::string& name,const std::string& text) {
    const auto temporary=".codestudio-agent-"+new_id();
    Fd file(openat(directory.get(),temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if (file.get()<0) fail("Create temporary LaunchAgent registration");
    try {
        std::size_t offset=0;
        while (offset<text.size()) {
            const auto count=write(file.get(),text.data()+offset,text.size()-offset);
            if (count<0) { if (errno==EINTR) continue; fail("Write LaunchAgent registration"); }
            if (!count) throw std::runtime_error("Cannot make progress writing LaunchAgent registration");
            offset+=static_cast<std::size_t>(count);
        }
        if (fsync(file.get())!=0) fail("Sync LaunchAgent registration");
        struct stat info{};
        if (fstat(file.get(),&info)!=0) fail("Inspect temporary LaunchAgent registration");
        same_entry(directory.get(),temporary,info); directory.verify();
        // Atomic, no replacement and no transient hard link to the published file.
        if (renameatx_np(directory.get(),temporary.c_str(),directory.get(),name.c_str(),RENAME_EXCL)!=0)
            fail("Publish LaunchAgent registration without replacement");
        if (fsync(directory.get())!=0) fail("Sync LaunchAgent directory");
        directory.verify();
    } catch (...) { (void)unlinkat(directory.get(),temporary.c_str(),0); throw; }
}
}
bool macos_launchd_disabled_override(std::string_view output,const std::string& label) {
    const auto trim=[](std::string_view value) {
        const auto first=value.find_first_not_of(" \t\r\n");
        if (first==std::string_view::npos) return std::string_view{};
        return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
    };
    output=trim(output);
    constexpr std::string_view prefix="disabled services = {";
    if (!output.starts_with(prefix) || !output.ends_with('}'))
        throw std::runtime_error("Unrecognized launchd disabled-services response");
    output=output.substr(prefix.size(),output.size()-prefix.size()-1);
    bool found=false,disabled=false;
    while (!output.empty()) {
        const auto end=output.find('\n');
        const auto line=trim(output.substr(0,end));
        output=end==std::string_view::npos?std::string_view{}:output.substr(end+1);
        if (line.empty()) continue;
        const auto separator=line.rfind("\" => ");
        if (!line.starts_with('"') || separator==std::string_view::npos || separator<2)
            throw std::runtime_error("Unrecognized launchd disabled-services entry");
        const auto value=line.substr(separator+5);
        if (value!="disabled" && value!="enabled" && value!="true" && value!="false")
            throw std::runtime_error("Unrecognized launchd disabled-services state");
        if (line.substr(1,separator-1)==label) {
            if (found) throw std::runtime_error("Duplicate launchd disabled-services entry");
            found=true; disabled=value=="disabled" || value=="true";
        }
    }
    return disabled;
}
Json macos_autostart_plan(const fs::path& executable,const fs::path& workspace,const fs::path& database,
                         const std::string& uid,const AutostartOptions& options) {
    validate_autostart_options(options);
    unsigned long long parsed=0;
    const auto [end,error]=std::from_chars(uid.data(),uid.data()+uid.size(),parsed);
    if (uid.empty() || error!=std::errc{} || end!=uid.data()+uid.size() || parsed>UINT32_MAX || std::to_string(parsed)!=uid)
        throw std::invalid_argument("Autostart user identity must be a numeric UID");
    for (const auto* path:{&executable,&workspace,&database})
        if (!path->is_absolute() || path->lexically_normal()!=*path) throw std::invalid_argument("Autostart paths must be normalized absolute paths");
    const auto exe=path_utf8(executable),directory=path_utf8(workspace),state=path_utf8(database);
    const auto label="com.codestudio.agent.scheduler."+sha256_bytes(Json::array({uid,directory,state}).dump());
    Json args=Json::array({exe,options.supervise?"scheduler-supervise":"scheduler-run","--workspace",directory,
        "--state",state,"--poll-ms",std::to_string(options.poll_ms)});
    if (options.supervise) {
        args.push_back("--max-restarts"); args.push_back(std::to_string(options.max_restarts));
        args.push_back("--restart-delay-ms"); args.push_back(std::to_string(options.restart_delay_ms));
    }
    std::string text="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n  <key>Label</key><string>"+xml(label)+"</string>\n  <key>ProgramArguments</key>\n  <array>\n";
    for (const auto& arg:args) text+="    <string>"+xml(arg.get<std::string>())+"</string>\n";
    text+="  </array>\n  <key>WorkingDirectory</key><string>"+xml(directory)+"</string>\n"
        "  <key>RunAtLoad</key><true/>\n  <key>KeepAlive</key><false/>\n"
        "  <key>LimitLoadToSessionType</key><string>Aqua</string>\n"
        "  <key>StandardInPath</key><string>/dev/null</string>\n"
        "  <key>StandardOutPath</key><string>/dev/null</string>\n"
        "  <key>StandardErrorPath</key><string>/dev/null</string>\n</dict>\n</plist>\n";
    Json result{{"task_name",label},{"user_uid",uid},{"workspace",directory},{"state_database",state},
        {"executable",exe},{"arguments",args},{"plist_text",text}};
    describe_options(result,options); return result;
}
bool macos_autostart_matches(const Json& expected,const Json& observed,bool check_options) {
    try {
        const auto requested=json_options(expected),stored=json_options(observed);
        return observed==macos_autostart_plan(expected.at("executable").get<std::string>(),expected.at("workspace").get<std::string>(),
            expected.at("state_database").get<std::string>(),expected.at("user_uid").get<std::string>(),check_options?requested:stored);
    } catch (const std::exception&) { return false; }
}
Json macos_autostart(const fs::path& workspace,const fs::path& database,std::string_view action,const AutostartOptions& options) {
    validate_autostart_options(options);
    const auto directory=fs::canonical(workspace);
    if (!fs::is_directory(directory)) throw std::invalid_argument("Autostart workspace must be a directory");
    const auto state=fs::weakly_canonical(fs::absolute(database));
    const auto expected=macos_autostart_plan(executable_path(),directory,state,std::to_string(geteuid()),options);
    const auto label=expected.at("task_name").get<std::string>(),name=label+".plist";
    const auto base=home_directory()/"Library"/"LaunchAgents";
    bool probe_available=false,disabled=false;
    try {
        ProcessRequest request;
        request.executable="/bin/launchctl"; request.arguments={"print-disabled","gui/"+std::to_string(geteuid())};
        request.working_directory="/"; request.timeout=std::chrono::milliseconds(2000); request.max_output_bytes=262144;
        const auto probe=run_process(request,std::atomic_bool{false});
        if (probe.exit_code==0 && !probe.timed_out && !probe.cancelled && !probe.output_truncated) {
            disabled=macos_launchd_disabled_override(probe.output,label); probe_available=true;
        }
    } catch (const std::exception&) {}
    Json result{{"available",true},{"registered",false},{"enabled",false},{"conflict",false},{"task_name",label},
        {"workspace",expected.at("workspace")},{"state_database",expected.at("state_database")},
        {"executable",expected.at("executable")},{"user_uid",expected.at("user_uid")},{"backend","launchagent"},
        {"trigger","current_user_logon"},{"started",false},{"plist_path",path_utf8(base/name)},{"requires_login",false},
        {"launchd_probe_available",probe_available},{"disabled_override",probe_available?Json(disabled):Json(nullptr)}};
    if (!probe_available) result["reason"]="Current-user GUI launchd overrides could not be verified; login enablement is unavailable";
    else if (disabled) result["reason"]="Current-user launchd has a persistent disabled override for this registration";
    // Never alter persistent OS overrides, nor claim enablement from the plist alone.
    if (action=="enable" && (!probe_available || disabled)) throw std::runtime_error(result.at("reason").get<std::string>());
    describe_options(result,options);
    auto agents=open_directory(base,action=="enable");
    // A directory lock serializes mutations without creating files on read-only paths.
    if (action!="status" && agents.get()>=0 && flock(agents.get(),LOCK_EX)!=0) fail("Lock LaunchAgent directory");
    agents.verify();
    const auto saved=read_registration(agents.get(),name);
    bool owned=false; AutostartOptions stored;
    if (saved.exists) {
        try {
            stored=plist_options(saved.text);
            owned=saved.text==macos_autostart_plan(expected.at("executable").get<std::string>(),directory,state,
                expected.at("user_uid").get<std::string>(),stored).at("plist_text").get<std::string>();
        } catch (const std::exception&) {}
    }
    result["registered"]=saved.exists; result["enabled"]=saved.exists && owned && probe_available && !disabled;
    result["conflict"]=(saved.exists && !owned) || disabled; result["requires_login"]=saved.exists && owned;
    if (owned) describe_options(result,stored);
    if (action=="status") return result;
    if ((saved.exists && !owned) || (action=="enable" && saved.exists && saved.text!=expected.at("plist_text").get<std::string>()))
        throw std::runtime_error("Autostart registration conflicts with this executable or configuration; no task was changed");
    if (action=="disable") {
        if (saved.exists) {
            agents.verify(); same_entry(agents.get(),name,saved.info);
            if (unlinkat(agents.get(),name.c_str(),0)!=0 || fsync(agents.get())!=0) fail("Remove owned LaunchAgent registration");
        }
        // Deliberately no launchctl bootout: a running worker is stopped only by scheduler-stop.
        result["registered"]=false; result["enabled"]=false; result["removed"]=saved.exists; result["requires_login"]=false;
        return result;
    }
    if (!saved.exists) write_registration(agents,name,expected.at("plist_text").get<std::string>());
    if (read_registration(agents.get(),name).text!=expected.at("plist_text").get<std::string>())
        throw std::runtime_error("LaunchAgent registration changed during readback");
    // No bootstrap/kickstart/load. launchd discovers this plist at the next GUI login.
    result["registered"]=true; result["enabled"]=true; result["created"]=!saved.exists; result["requires_login"]=true;
    describe_options(result,options); return result;
}
}
#endif
