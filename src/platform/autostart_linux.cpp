#include "autostart_linux.hpp"

#ifdef __linux__
#include "codestudio/digest.hpp"
#include "codestudio/store.hpp"
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <fcntl.h>
#include <pwd.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace codestudio {
namespace {
namespace fs=std::filesystem;
constexpr auto marker="# CodeStudio-Agent managed current-user scheduler\n";
class Fd {
    int value_=-1;
public:
    explicit Fd(int value=-1):value_(value) {}
    ~Fd() { if (value_>=0) close(value_); }
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other) noexcept:value_(other.value_) { other.value_=-1; }
    int get() const { return value_; }
    void reset(int value) { if (value_>=0) close(value_); value_=value; }
};
[[noreturn]] void fail(const char* operation) { throw std::system_error(errno,std::generic_category(),operation); }
std::string quoted(const std::string& value) {
    if (value.empty() || value.size()>32768) throw std::invalid_argument("Invalid systemd argument length");
    std::string result="\"";
    for (const unsigned char ch:value) {
        if (ch<32 || ch==127) throw std::invalid_argument("Systemd arguments cannot contain control bytes");
        if (ch=='%' ) result+='%'; // Escape specifiers in every systemd directive.
        if (ch=='\\' || ch=='\"') result+='\\';
        result+=static_cast<char>(ch);
    }
    return result+'\"';
}
std::string working_directory(const std::string& value) {
    // WorkingDirectory consumes the whole value, not an ExecStart argument.
    // Quotes/backslashes there are literal, while percent specifiers still expand.
    (void)quoted(value);
    if (value.back()==' ' || value.back()=='\\')
        throw std::invalid_argument("Systemd working directory cannot end with a space or backslash");
    std::string result;
    for (const auto ch:value) { if (ch=='%') result+='%'; result+=ch; }
    return result;
}
int parse_number(std::string_view text) {
    int result=0;
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),result);
    if (error!=std::errc{} || end!=text.data()+text.size() || std::to_string(result)!=text)
        throw std::invalid_argument("Invalid registered autostart integer");
    return result;
}
AutostartOptions unit_options(std::string_view text) {
    if (!text.starts_with(marker)) throw std::runtime_error("Foreign autostart unit");
    text.remove_prefix(std::string_view(marker).size());
    const auto number=[&](std::string_view prefix) {
        if (!text.starts_with(prefix)) throw std::runtime_error("Malformed autostart metadata");
        text.remove_prefix(prefix.size());
        const auto end=text.find('\n');
        if (end==std::string_view::npos) throw std::runtime_error("Malformed autostart unit");
        const auto result=parse_number(text.substr(0,end));
        text.remove_prefix(end+1);
        return result;
    };
    AutostartOptions options;
    options.poll_ms=number("# PollMS=");
    constexpr std::string_view supervised="# Supervised=true\n";
    if (text.starts_with(supervised)) {
        options.supervise=true;
        text.remove_prefix(supervised.size());
        options.max_restarts=number("# MaxRestarts=");
        options.restart_delay_ms=number("# RestartDelayMS=");
    }
    validate_autostart_options(options);
    return options;
}
AutostartOptions json_options(const Json& plan) {
    const auto number=[&](const char* key) {
        const auto& value=plan.at(key);
        // JSON numeric equality permits floating-point coercion; ownership must not.
        if (!value.is_number_integer() || value<0 || value>60000)
            throw std::invalid_argument("Invalid autostart integer metadata");
        return value.get<int>();
    };
    AutostartOptions options;
    options.poll_ms=number("poll_ms");
    options.supervise=plan.at("supervised").get<bool>();
    if (options.supervise) {
        options.max_restarts=number("max_restarts");
        options.restart_delay_ms=number("restart_delay_ms");
    } else if (plan.contains("max_restarts") || plan.contains("restart_delay_ms")) {
        throw std::invalid_argument("Unsupervised autostart has restart metadata");
    }
    validate_autostart_options(options);
    return options;
}
void describe_options(Json& result,const AutostartOptions& options) {
    result["poll_ms"]=options.poll_ms;
    result["supervised"]=options.supervise;
    if (options.supervise) {
        result["max_restarts"]=options.max_restarts;
        result["restart_delay_ms"]=options.restart_delay_ms;
    } else {
        result.erase("max_restarts");
        result.erase("restart_delay_ms");
    }
}
std::string home_directory() {
    const auto* entry=getpwuid(geteuid());
    if (!entry || !entry->pw_dir || !*entry->pw_dir) throw std::runtime_error("Cannot resolve current-user home directory");
    return entry->pw_dir;
}
fs::path config_directory() {
    const auto* configured=std::getenv("XDG_CONFIG_HOME");
    fs::path path=configured && *configured ? fs::path(configured) : fs::path(home_directory())/".config";
    if (!path.is_absolute() || path.lexically_normal()!=path)
        throw std::runtime_error("XDG_CONFIG_HOME must be an absolute normalized path");
    (void)quoted(path.string()); return path;
}
Fd open_directory(const fs::path& path,bool create) {
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd<0) fail("Open autostart filesystem root");
    for (const auto& component:path.relative_path()) {
        const auto name=component.string();
        if (name.empty() || name=="." || name=="..") { close(fd); throw std::runtime_error("Unsafe autostart directory component"); }
        auto next=openat(fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if (next<0 && errno==ENOENT && create) {
            if (mkdirat(fd,name.c_str(),0700)!=0 && errno!=EEXIST) { const auto error=errno; close(fd); errno=error; fail("Create autostart directory"); }
            next=openat(fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        }
        if (next<0) {
            const auto error=errno; close(fd);
            if (error==ENOENT && !create) return Fd();
            errno=error; fail("Open anchored autostart directory");
        }
        close(fd); fd=next;
    }
    struct stat info{};
    if (fstat(fd,&info)!=0) { const auto error=errno; close(fd); errno=error; fail("Inspect autostart directory"); }
    if (info.st_uid!=geteuid() || (info.st_mode&0022)) {
        close(fd); throw std::runtime_error("Autostart configuration directory must be owned by the current user and not writable by others");
    }
    return Fd(fd);
}
struct Unit { bool exists=false; std::string contents; dev_t device=0; ino_t inode=0; };
Unit read_unit(int directory,const std::string& name) {
    if (directory<0) return {};
    const auto raw=openat(directory,name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (raw<0) { if (errno==ENOENT) return {}; fail("Open autostart unit"); }
    Fd file(raw); struct stat info{};
    if (fstat(file.get(),&info)!=0) fail("Inspect autostart unit");
    if (!S_ISREG(info.st_mode) || info.st_uid!=geteuid() || info.st_nlink!=1 || (info.st_mode&0022) || info.st_size>262144)
        throw std::runtime_error("Autostart unit ownership, type or size conflicts with the expected registration");
    Unit result{true,{},info.st_dev,info.st_ino};
    char buffer[4096];
    for (;;) {
        const auto count=read(file.get(),buffer,sizeof buffer);
        if (count<0) { if (errno==EINTR) continue; fail("Read autostart unit"); }
        if (!count) break;
        if (result.contents.size()+count>262144) throw std::runtime_error("Autostart unit exceeds its read limit");
        result.contents.append(buffer,static_cast<std::size_t>(count));
    }
    return result;
}
struct Link { bool exists=false; bool owned=false; dev_t device=0; ino_t inode=0; };
Link read_link(int directory,const std::string& name) {
    if (directory<0) return {};
    struct stat info{};
    if (fstatat(directory,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)!=0) {
        if (errno==ENOENT) return {}; fail("Inspect autostart enable link");
    }
    Link result{true,false,info.st_dev,info.st_ino};
    if (!S_ISLNK(info.st_mode) || info.st_uid!=geteuid()) return result;
    char target[4096]; const auto length=readlinkat(directory,name.c_str(),target,sizeof target);
    if (length<0) fail("Read autostart enable link");
    result.owned=std::string_view(target,static_cast<std::size_t>(length))=="../"+name;
    return result;
}
void same_entry(int directory,const std::string& name,dev_t device,ino_t inode) {
    struct stat current{};
    if (fstatat(directory,name.c_str(),&current,AT_SYMLINK_NOFOLLOW)!=0 || current.st_dev!=device || current.st_ino!=inode)
        throw std::runtime_error("Autostart registration changed during control; no replacement was removed");
}
void write_unit(int directory,const std::string& name,const std::string& text) {
    const auto temporary=".codestudio-agent-"+new_id();
    Fd file(openat(directory,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if (file.get()<0) fail("Create temporary autostart unit");
    try {
        std::size_t offset=0;
        while (offset<text.size()) {
            const auto count=write(file.get(),text.data()+offset,text.size()-offset);
            if (count<0) { if (errno==EINTR) continue; fail("Write autostart unit"); }
            offset+=static_cast<std::size_t>(count);
        }
        if (fsync(file.get())!=0) fail("Sync autostart unit");
        // linkat is atomic and cannot replace a competing or foreign unit.
        if (linkat(directory,temporary.c_str(),directory,name.c_str(),0)!=0) fail("Publish autostart unit without replacement");
        if (unlinkat(directory,temporary.c_str(),0)!=0) fail("Remove autostart temporary link");
        if (fsync(directory)!=0) fail("Sync autostart unit directory");
    } catch (...) { (void)unlinkat(directory,temporary.c_str(),0); throw; }
}
}

Json linux_autostart_plan(const fs::path& executable,const fs::path& workspace,const fs::path& database,
                         const std::string& uid,const AutostartOptions& options) {
    validate_autostart_options(options);
    unsigned long long parsed_uid=0;
    const auto [end,error]=std::from_chars(uid.data(),uid.data()+uid.size(),parsed_uid);
    if (uid.empty() || error!=std::errc{} || end!=uid.data()+uid.size() || parsed_uid>UINT32_MAX || std::to_string(parsed_uid)!=uid)
        throw std::invalid_argument("Autostart user identity must be a numeric UID");
    for (const auto* path:{&executable,&workspace,&database})
        if (!path->is_absolute() || path->lexically_normal()!=*path)
            throw std::invalid_argument("Autostart paths must be normalized absolute paths");
    const auto exe=path_utf8(executable),directory=path_utf8(workspace),state=path_utf8(database);
    const auto unit="codestudio-agent-"+sha256_bytes(Json::array({uid,directory,state}).dump())+".service";
    std::string contents=std::string(marker)+"# PollMS="+std::to_string(options.poll_ms)+"\n";
    if (options.supervise) contents+="# Supervised=true\n# MaxRestarts="+std::to_string(options.max_restarts)+
        "\n# RestartDelayMS="+std::to_string(options.restart_delay_ms)+"\n";
    contents+="[Unit]\nDescription=CodeStudio-Agent current-user scheduler\n\n[Service]\nType=exec\nExecStart=:"+
        quoted(exe)+(options.supervise ? " scheduler-supervise" : " scheduler-run")+
        " --workspace "+quoted(directory)+" --state "+quoted(state)+" --poll-ms "+std::to_string(options.poll_ms);
    if (options.supervise) contents+=" --max-restarts "+std::to_string(options.max_restarts)+
        " --restart-delay-ms "+std::to_string(options.restart_delay_ms);
    contents+="\nWorkingDirectory="+working_directory(directory)+
        "\nRestart=no\nStandardInput=null\nStandardOutput=null\nStandardError=journal\nKillMode=control-group"
        "\nTimeoutStopSec=30\n\n[Install]\nWantedBy=default.target\n";
    Json result{{"task_name",unit},{"user_uid",uid},{"workspace",directory},{"state_database",state},
        {"executable",exe},{"unit_text",contents}};
    describe_options(result,options);
    return result;
}
bool linux_autostart_matches(const Json& expected,const Json& observed,bool check_options) {
    try {
        const auto stored=json_options(observed);
        const auto requested=json_options(expected);
        const auto exact=linux_autostart_plan(expected.at("executable").get<std::string>(),
            expected.at("workspace").get<std::string>(),expected.at("state_database").get<std::string>(),
            expected.at("user_uid").get<std::string>(),check_options ? requested : stored);
        return exact==observed;
    } catch (const std::exception&) { return false; }
}
Json linux_autostart(const fs::path& workspace,const fs::path& database,std::string_view action,const AutostartOptions& options) {
    validate_autostart_options(options);
    const auto directory=fs::canonical(workspace);
    if (!fs::is_directory(directory)) throw std::invalid_argument("Autostart workspace must be a directory");
    const auto state=fs::weakly_canonical(fs::absolute(database));
    const auto expected=linux_autostart_plan(fs::canonical("/proc/self/exe"),directory,state,std::to_string(geteuid()),options);
    const auto name=expected.at("task_name").get<std::string>();
    const auto base=config_directory()/"systemd"/"user";
    const bool available=fs::is_directory("/run/systemd/system") && access("/usr/bin/systemctl",X_OK)==0;
    Json result{{"available",available},{"registered",false},{"enabled",false},{"conflict",false},
        {"task_name",name},{"workspace",expected.at("workspace")},{"state_database",expected.at("state_database")},
        {"executable",expected.at("executable")},{"user_uid",expected.at("user_uid")},{"poll_ms",options.poll_ms},
        {"supervised",false},
        {"trigger","user_manager_start"},{"backend","systemd-user"},{"started",false},
        {"unit_path",path_utf8(base/name)},{"requires_manager_reload",false}};
    describe_options(result,options);
    if (!available) result["reason"]="Linux login autostart requires systemd and /usr/bin/systemctl";
    if (!available && action=="enable") throw std::runtime_error(result.at("reason").get<std::string>());
    const bool create=action=="enable";
    auto units=open_directory(base,create);
    auto wants=open_directory(base/"default.target.wants",create);
    Fd lock;
    // Lock only mutations. Read-only status must not create a lock or directory.
    if (action!="status" && units.get()>=0) {
        const auto raw=openat(units.get(),".codestudio-agent.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if (raw<0) fail("Open autostart registration lock");
        struct stat info{};
        if (fstat(raw,&info)!=0 || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() || info.st_nlink!=1 || (info.st_mode&0077)) {
            close(raw); throw std::runtime_error("Unsafe autostart registration lock");
        }
        lock.reset(raw);
        if (flock(raw,LOCK_EX)!=0) fail("Lock autostart registration");
    }
    const auto saved=read_unit(units.get(),name);
    const auto link=read_link(wants.get(),name);
    bool owned=false; AutostartOptions stored;
    if (saved.exists) {
        try {
            stored=unit_options(saved.contents);
            const auto actual=linux_autostart_plan(fs::path(expected.at("executable").get<std::string>()),directory,state,
                expected.at("user_uid").get<std::string>(),stored);
            owned=actual.at("unit_text").get<std::string>()==saved.contents;
        } catch (const std::exception&) {}
    }
    result["registered"]=saved.exists;
    result["enabled"]=saved.exists && owned && link.owned;
    result["conflict"]=(saved.exists && !owned) || (link.exists && (!saved.exists || !link.owned));
    if (owned) describe_options(result,stored);
    if (action=="status") return result;
    if (result.at("conflict").get<bool>() || (action=="enable" && saved.exists &&
        saved.contents!=expected.at("unit_text").get<std::string>()))
        throw std::runtime_error("Autostart registration conflicts with this executable or configuration; no task was changed");
    if (action=="disable") {
        if (link.exists) {
            same_entry(wants.get(),name,link.device,link.inode);
            if (unlinkat(wants.get(),name.c_str(),0)!=0 || fsync(wants.get())!=0) fail("Remove autostart enable link");
        }
        if (saved.exists) {
            same_entry(units.get(),name,saved.device,saved.inode);
            if (unlinkat(units.get(),name.c_str(),0)!=0 || fsync(units.get())!=0) fail("Remove owned autostart unit");
        }
        result["registered"]=false; result["enabled"]=false; result["removed"]=saved.exists;
        result["requires_manager_reload"]=saved.exists; return result;
    }
    if (!saved.exists) write_unit(units.get(),name,expected.at("unit_text").get<std::string>());
    if (!link.exists) {
        const auto target="../"+name;
        if (symlinkat(target.c_str(),wants.get(),name.c_str())!=0 || fsync(wants.get())!=0) fail("Enable owned autostart unit");
    }
    const auto verify=read_unit(units.get(),name);
    if (verify.contents!=expected.at("unit_text").get<std::string>() || !read_link(wants.get(),name).owned)
        throw std::runtime_error("Autostart registration changed during readback");
    result["registered"]=true; result["enabled"]=true; result["created"]=!saved.exists;
    describe_options(result,options);
    result["requires_manager_reload"]=!saved.exists || !link.exists;
    return result;
}
}
#endif
