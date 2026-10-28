#ifdef __linux__
#include "codestudio/sandbox_resources.hpp"
#include "codestudio/process_observation.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace codestudio {
namespace {
namespace fs=std::filesystem;
constexpr std::size_t record_limit=65536;
constexpr std::size_t ancestry_limit=128;
[[noreturn]] void fail(const std::string& message) {throw std::runtime_error("sandbox resources: "+message);}
[[noreturn]] void posix_fail(const char* operation) {fail(std::string(operation)+" (errno "+std::to_string(errno)+")");}
void require(bool value,const char* message) {if(!value)fail(message);}
void cancelled_check(const std::atomic_bool& cancelled) {if(cancelled.load())fail("cancelled");}
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd){
        if(fd>=0&&fd<3){value=fcntl(fd,F_DUPFD_CLOEXEC,3);const auto error=errno;close(fd);if(value<0){errno=error;posix_fail("move resource descriptor above stdio");}}
    }
    ~Fd(){if(value>=0)close(value);}
    Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other) noexcept:value(std::exchange(other.value,-1)){}
    Fd& operator=(Fd&& other) noexcept {if(this!=&other){if(value>=0)close(value);value=std::exchange(other.value,-1);}return *this;}
};
struct stat metadata(int fd) {struct stat value{};if(fstat(fd,&value))posix_fail("inspect descriptor");return value;}
bool same(const struct stat& a,const struct stat& b) {return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&(a.st_mode&S_IFMT)==(b.st_mode&S_IFMT);}
void component(const std::string& value) {
    require(!value.empty()&&value!="."&&value!=".."&&value.find_first_of("/\0",0,2)==std::string::npos,"invalid path component");
}
fs::path checked_path(const fs::path& path) {
    const auto& name=path.native();
    require(path.is_absolute()&&name.size()<=4096&&name.find('\0')==std::string::npos&&
        path.lexically_normal().native()==name&&name.find("//")==std::string::npos,"expected a normalized absolute path");
    for(const auto& part:path.relative_path())component(part.native());
    return path;
}
void valid_id(const std::string& id) {
    require(id.size()==32&&std::ranges::all_of(id,[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"invalid sandbox resource id");
}
void verify_entry(int parent,const std::string& name,int fd) {
    struct stat current{};
    if(fstatat(parent,name.c_str(),&current,AT_SYMLINK_NOFOLLOW))posix_fail("revalidate directory entry");
    require(same(current,metadata(fd)),"directory entry changed");
}
Json identity(int fd) {
    const auto value=metadata(fd);
    struct statx extended{};
    if(statx(fd,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_BASIC_STATS|STATX_BTIME|STATX_MNT_ID,&extended))
        posix_fail("inspect native directory identity");
    require((extended.stx_mask&STATX_MNT_ID)!=0,"native mount identity is unavailable");
    return {{"dev",static_cast<std::uint64_t>(value.st_dev)},{"ino",static_cast<std::uint64_t>(value.st_ino)},
        {"mount",extended.stx_mnt_id},{"birth_seconds",(extended.stx_mask&STATX_BTIME)?Json(extended.stx_btime.tv_sec):Json(nullptr)},
        {"birth_nanoseconds",(extended.stx_mask&STATX_BTIME)?Json(extended.stx_btime.tv_nsec):Json(nullptr)}};
}
struct Anchor {
    std::vector<Fd> fds;
    std::vector<std::string> names;
    bool missing=false;
    explicit Anchor(const fs::path& path,bool create=false,bool allow_missing=false) {
        checked_path(path);
        Fd root(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));if(root.value<0)posix_fail("open root");
        fds.push_back(std::move(root));
        for(const auto& part:path.relative_path()) {
            const auto name=part.native();
            if(create&&mkdirat(fd(),name.c_str(),0700)&&errno!=EEXIST)posix_fail("create private directory");
            Fd next(openat(fd(),name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
            if(next.value<0) {if(allow_missing&&errno==ENOENT){missing=true;return;}posix_fail("open anchored directory");}
            verify_entry(fd(),name,next.value);names.push_back(name);fds.push_back(std::move(next));
        }
    }
    int fd() const {return fds.back().value;}
    void verify() const {for(std::size_t i=0;i<names.size();++i)verify_entry(fds[i].value,names[i],fds[i+1].value);}
    Json chain() const {
        Json result=Json::array();fs::path path="/";
        result.push_back({{"path",path.native()},{"identity",identity(fds.front().value)}});
        for(std::size_t i=0;i<names.size();++i){path/=names[i];result.push_back({{"path",path.native()},{"identity",identity(fds[i+1].value)}});}
        return result;
    }
};
void private_directory(int fd) {
    const auto value=metadata(fd);
    require(S_ISDIR(value.st_mode)&&value.st_uid==geteuid()&&(value.st_mode&0777)==0700,"registry directory must be private and owned by the current user");
}
void private_file(int fd) {
    const auto value=metadata(fd);
    require(S_ISREG(value.st_mode)&&value.st_nlink==1&&value.st_uid==geteuid()&&(value.st_mode&0777)==0600,"registry file must be private, ordinary and singly linked");
}
fs::path registry_path() {
    if(const auto* configured=std::getenv("CODESTUDIO_SANDBOX_REGISTRY");configured&&*configured)return checked_path(configured);
    if(const auto* state=std::getenv("XDG_STATE_HOME");state&&*state)return checked_path(fs::path(state)/"CodeStudio-Agent/sandbox-resources");
    const auto* home=std::getenv("HOME");require(home&&*home,"HOME or XDG_STATE_HOME is required for resource registration");
    return checked_path(fs::path(home)/".local/state/CodeStudio-Agent/sandbox-resources");
}
std::vector<std::string> entries(int fd,std::size_t limit=4096,const std::atomic_bool* cancelled=nullptr) {
    Fd copy(openat(fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));if(copy.value<0)posix_fail("open enumeration");
    DIR* directory=fdopendir(copy.value);if(!directory)posix_fail("enumerate directory");copy.value=-1;
    struct Close {DIR* value;~Close(){closedir(value);}} close{directory};
    std::vector<std::string> result;
    for(;;){if(cancelled)cancelled_check(*cancelled);errno=0;const auto* item=readdir(directory);if(!item){if(errno)posix_fail("read directory");break;}
        std::string name=item->d_name;if(name=="."||name=="..")continue;component(name);
        require(result.size()<limit,"directory entry budget exceeded");result.push_back(std::move(name));}
    std::ranges::sort(result);return result;
}
std::string read_small(int parent,const char* name,std::size_t maximum,bool secure) {
    Fd file(openat(parent,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));if(file.value<0)posix_fail("open resource record");
    if(secure)private_file(file.value);
    const auto before=metadata(file.value);require(S_ISREG(before.st_mode),"resource metadata must be a regular file");
    std::string data;std::array<char,4096> buffer{};
    for(;;){auto count=read(file.value,buffer.data(),buffer.size());if(count<0){if(errno==EINTR)continue;posix_fail("read resource record");}
        if(!count)break;require(static_cast<std::size_t>(count)<=maximum-data.size(),"resource record exceeds byte limit");data.append(buffer.data(),count);}
    verify_entry(parent,name,file.value);
    const auto after=metadata(file.value);
    require(same(before,after)&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&
        before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,
        "resource record changed while reading");
    return data;
}
void write_record(int parent,const Json& value) {
    const auto bytes=value.dump()+"\n";require(bytes.size()<=record_limit,"resource record exceeds byte limit");
    // An interrupted prior update can leave its private temporary file. The
    // caller holds the resource lease, so only that exact ordinary file is
    // eligible for removal; the durable record remains the authority.
    {
        Fd prior(openat(parent,"record.tmp",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
        if(prior.value>=0){private_file(prior.value);verify_entry(parent,"record.tmp",prior.value);
            if(unlinkat(parent,"record.tmp",0))posix_fail("remove interrupted record update");}
        else if(errno!=ENOENT)posix_fail("inspect interrupted record update");
    }
    Fd file(openat(parent,"record.tmp",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if(file.value<0)posix_fail("create resource record update");
    private_file(file.value);
    std::size_t at=0;while(at<bytes.size()){auto count=write(file.value,bytes.data()+at,bytes.size()-at);if(count<0){if(errno==EINTR)continue;posix_fail("write resource record");}require(count>0,"short resource record write");at+=count;}
    if(fsync(file.value))posix_fail("persist resource record");
    verify_entry(parent,"record.tmp",file.value);
    if(renameat(parent,"record.tmp",parent,"record.json"))posix_fail("publish resource record");
    if(fsync(parent))posix_fail("persist resource entry");
}
void exact(const Json& value,std::initializer_list<const char*> keys) {
    require(value.is_object()&&value.size()==keys.size(),"unexpected resource record fields");
    for(auto key:keys)require(value.contains(key),"missing resource record field");
}
bool clean_string(const Json& value,std::size_t maximum,bool empty=false) {
    return value.is_string()&&value.get_ref<const std::string&>().size()<=maximum&&
        (empty||!value.get_ref<const std::string&>().empty())&&value.get_ref<const std::string&>().find('\0')==std::string::npos;
}
void valid_identity(const Json& value) {
    exact(value,{"dev","ino","mount","birth_seconds","birth_nanoseconds"});
    for(auto key:{"dev","ino","mount"})require(value[key].is_number_unsigned(),"invalid native resource identity");
    require((value["birth_seconds"].is_null()&&value["birth_nanoseconds"].is_null())||
        (value["birth_seconds"].is_number_integer()&&value["birth_nanoseconds"].is_number_unsigned()&&value["birth_nanoseconds"]<1000000000),
        "invalid native birth identity");
}
void resource_path(const std::string& kind,const fs::path& path,const std::string& id) {
    checked_path(path);
    const auto relative=path.relative_path();
    require(1+static_cast<std::size_t>(std::distance(relative.begin(),relative.end()))<=ancestry_limit,
        "resource ancestry exceeds limit");
    require(kind=="staging"||kind=="cgroup","unsupported Linux resource kind");
    require(path.filename()=="codestudio-isolation-"+id,"resource path does not match registered id");
    if(kind=="cgroup")require(path.native().starts_with("/sys/fs/cgroup/")&&path.parent_path()!=fs::path("/sys/fs/cgroup"),"resource cgroup must be below a delegation");
    else require(!path.native().starts_with("/sys/")&&!path.native().starts_with("/proc/")&&path.parent_path()!=fs::path("/"),"invalid staging location");
}
Json parse_record(const std::string& text,const std::string& id) {
    std::vector<std::set<std::string>> objects;
    auto record=Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value){
        require(depth<=32,"resource record nesting exceeds limit");
        if(event==Json::parse_event_t::object_start)objects.emplace_back();
        if(event==Json::parse_event_t::key)require(!objects.empty()&&objects.back().insert(value.get<std::string>()).second,"duplicate resource record key");
        if(event==Json::parse_event_t::object_end)objects.pop_back();return true;});
    exact(record,{"format","id","backend","kind","workspace","task_id","owner","phase","resources"});
    require(record["format"].is_number_integer()&&record["format"]==2&&record["id"]==id&&record["backend"]=="linux-namespace","unsupported resource record identity");
    require(record["kind"]=="process"||record["kind"]=="package"||record["kind"]=="build","invalid resource purpose");
    require(clean_string(record["workspace"],4096)&&record["workspace"].get<std::string>().front()=='/',"invalid resource workspace");
    require(record["task_id"].is_null()||clean_string(record["task_id"],128),"invalid resource task");
    require(record["phase"]=="preparing"||record["phase"]=="ready"||record["phase"]=="reclaiming","invalid resource phase");
    const auto& owner=record["owner"];exact(owner,{"pid","identity","uid"});
    require(owner["pid"].is_number_integer()&&owner["pid"]>0&&owner["pid"]<=2147483647&&clean_string(owner["identity"],128)&&
        owner["uid"].is_number_unsigned()&&owner["uid"]==geteuid(),"invalid resource owner");
    const auto& resources=record["resources"];
    require(resources.is_array()&&resources.size()<=2,"invalid resource inventory");std::set<std::string> kinds;
    for(const auto& resource:resources){
        exact(resource,{"kind","path","captured","chain"});
        require(clean_string(resource["kind"],16)&&clean_string(resource["path"],4096)&&resource["captured"].is_boolean()&&resource["chain"].is_array(),"invalid resource path record");
        const auto kind=resource["kind"].get<std::string>();const fs::path path(resource["path"].get<std::string>());
        resource_path(kind,path,id);require(kinds.insert(kind).second,"duplicate resource kind");
        const auto& chain=resource["chain"];
        if(!resource["captured"].get<bool>()){require(chain.empty(),"uncaptured resource contains an identity");continue;}
        require(!chain.empty()&&chain.size()<=ancestry_limit,"invalid resource ancestry");
        std::vector<fs::path> expected{"/"};fs::path prefix="/";
        for(const auto& part:path.relative_path()){prefix/=part;expected.push_back(prefix);}
        require(chain.size()==expected.size(),"incomplete resource ancestry");
        for(std::size_t i=0;i<chain.size();++i){exact(chain[i],{"path","identity"});require(chain[i]["path"]==expected[i].native(),"resource ancestry differs from path");valid_identity(chain[i]["identity"]);}
    }
    if(record["phase"]=="ready")require(resources.size()==2&&std::ranges::all_of(resources,[](const Json& item){return item["captured"].get<bool>();}),"ready resource inventory is incomplete");
    return record;
}
struct OpenRecord {
    Anchor registry;
    Fd directory,lock;
    bool locked=false;
    std::string id;
    explicit OpenRecord(const fs::path& path,const std::string& resource_id,bool allow_missing):registry(path,false,allow_missing),id(resource_id) {
        if(registry.missing)return;private_directory(registry.fd());
        directory=Fd(openat(registry.fd(),id.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        if(directory.value<0){if(allow_missing&&errno==ENOENT)return;posix_fail("open registered resource");}
        private_directory(directory.value);verify_entry(registry.fd(),id,directory.value);
        lock=Fd(openat(directory.value,"lease",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));if(lock.value<0)posix_fail("open registered lease");
        private_file(lock.value);verify_entry(directory.value,"lease",lock.value);
        if(flock(lock.value,LOCK_EX|LOCK_NB)==0)locked=true;
        else if(errno!=EWOULDBLOCK&&errno!=EAGAIN)posix_fail("observe resource lease");
    }
    void verify() const {registry.verify();verify_entry(registry.fd(),id,directory.value);verify_entry(directory.value,"lease",lock.value);}
    bool missing() const {return directory.value<0;}
};
struct ResourceView {Json report;std::unique_ptr<Anchor> anchor;};
ResourceView inspect_resource(const Json& resource,const Json& owner) {
    ResourceView result{resource,nullptr};const fs::path path(resource["path"].get<std::string>());
    result.anchor=std::make_unique<Anchor>(path,false,true);
    auto& anchor=*result.anchor;
    if(resource["captured"].get<bool>()) {
        const auto actual=anchor.chain();const auto& expected=resource["chain"];
        for(std::size_t i=0;i<actual.size();++i)require(actual[i]==expected[i],"resource directory identity changed");
    }
    if(anchor.missing){result.report["state"]="missing";return result;}
    require(resource["captured"].get<bool>(),"resource exists without a captured ownership identity");
    const auto state=metadata(anchor.fd());require(state.st_uid==geteuid(),"resource owner changed");
    if(resource["kind"]=="cgroup"){
        const auto current=current_process_identity();const auto recorded=owner["identity"].get<std::string>();
        require(current.size()>=43&&recorded.size()>=43&&current.substr(0,43)==recorded.substr(0,43),"cgroup belongs to a different or unknown system boot");
        const auto events=read_small(anchor.fd(),"cgroup.events",4096,false);
        require(events.find("populated 0\n")!=std::string::npos&&events.find("populated 1")==std::string::npos,"cgroup still contains processes or cannot be observed");
    }
    anchor.verify();result.report["state"]="present";return result;
}
struct Inspection {Json report;std::vector<ResourceView> resources;};
Inspection inspect(OpenRecord& opened,const Json& record) {
    Inspection result{record,{}};result.report["state"]="blocked";result.report["reason"]="";
    if(!opened.locked){result.report["state"]="active";result.report["reason"]="lease_held";return result;}
    opened.verify();
    const auto observation=observe_process_identity(record["owner"]["pid"].get<std::int64_t>(),record["owner"]["identity"].get<std::string>());
    result.report["owner_observation"]=observation;
    if(observation=="matching_process"){result.report["state"]="active";result.report["reason"]="owner_running";return result;}
    if(observation!="not_running"&&observation!="identity_mismatch"){result.report["reason"]="owner_unavailable";return result;}
    try {
        for(std::size_t i=0;i<record["resources"].size();++i){auto view=inspect_resource(record["resources"][i],record["owner"]);result.report["resources"][i]=view.report;result.resources.push_back(std::move(view));}
        opened.verify();result.report["state"]="reclaimable";result.report["reason"]="owner_absent_resources_quiescent";
    } catch(const std::exception& error){result.report["reason"]=error.what();}
    return result;
}
void remove_tree(int parent,const std::string& name,std::uint64_t mount,unsigned depth,std::size_t& budget,
                 const std::atomic_bool& cancelled,const Json* expected_root=nullptr) {
    cancelled_check(cancelled);require(depth<=64&&budget>0,"cleanup entry budget exceeded");--budget;
    struct stat before{};if(fstatat(parent,name.c_str(),&before,AT_SYMLINK_NOFOLLOW)){if(errno==ENOENT)return;posix_fail("inspect cleanup entry");}
    if(expected_root)require(S_ISDIR(before.st_mode),"registered cleanup root was replaced");
    if(S_ISDIR(before.st_mode)){
        Fd directory(openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));if(directory.value<0)posix_fail("open cleanup directory");
        require(same(before,metadata(directory.value)),"cleanup directory changed");
        const auto opened_identity=identity(directory.value);
        if(expected_root)require(opened_identity==*expected_root,"registered cleanup root identity changed before traversal");
        require(opened_identity["mount"]==mount,"cleanup refuses mounted directories");
        for(const auto& child:entries(directory.value,budget,&cancelled))remove_tree(directory.value,child,mount,depth+1,budget,cancelled);
        cancelled_check(cancelled);verify_entry(parent,name,directory.value);
        if(unlinkat(parent,name.c_str(),AT_REMOVEDIR))posix_fail("remove cleanup directory");
    }else{
        // unlinkat never traverses a symlink or modifies a hard-link target.
        cancelled_check(cancelled);struct stat current{};
        if(fstatat(parent,name.c_str(),&current,AT_SYMLINK_NOFOLLOW))posix_fail("revalidate cleanup file");
        require(same(before,current),"cleanup file changed");
        if(unlinkat(parent,name.c_str(),0))posix_fail("remove cleanup file");
    }
}
void remove_journal(OpenRecord& opened) {
    opened.verify();
    const auto names=entries(opened.directory.value,8);
    for(const auto& name:names)require(name=="record.json"||name=="record.tmp"||name=="lease","unexpected file in resource journal");
    for(const auto* name:{"record.tmp","record.json"}){
        Fd file(openat(opened.directory.value,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
        if(file.value<0){if(errno==ENOENT)continue;posix_fail("open completed resource record");}
        private_file(file.value);verify_entry(opened.directory.value,name,file.value);
        if(unlinkat(opened.directory.value,name,0))posix_fail("remove completed resource record");
    }
    verify_entry(opened.directory.value,"lease",opened.lock.value);
    if(unlinkat(opened.directory.value,"lease",0))posix_fail("remove completed lease");
    verify_entry(opened.registry.fd(),opened.id,opened.directory.value);
    if(unlinkat(opened.registry.fd(),opened.id.c_str(),AT_REMOVEDIR))posix_fail("remove completed resource journal");
    if(fsync(opened.registry.fd()))posix_fail("persist completed resource cleanup");
}
}

struct SandboxResourceLease::Impl {
    fs::path root;
    std::string id;
    Anchor registry;
    Fd directory,lock;
    Json record;
    bool finished=false;
    Impl(const std::string& supplied,const std::string& kind,const fs::path& workspace,const std::string& task):root(registry_path()),id(supplied),registry(root,true) {
        valid_id(id);require(kind=="process"||kind=="package"||kind=="build","invalid resource purpose");
        checked_path(workspace);require(task.size()<=128&&task.find('\0')==std::string::npos,"invalid resource task");
        private_directory(registry.fd());registry.verify();
        if(mkdirat(registry.fd(),id.c_str(),0700))posix_fail("create exclusive resource journal");
        directory=Fd(openat(registry.fd(),id.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));if(directory.value<0)posix_fail("open new resource journal");
        private_directory(directory.value);verify_entry(registry.fd(),id,directory.value);
        lock=Fd(openat(directory.value,"lease",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));if(lock.value<0)posix_fail("create resource lease");
        private_file(lock.value);if(flock(lock.value,LOCK_EX|LOCK_NB))posix_fail("lock new resource lease");
        record={{"format",2},{"id",id},{"backend","linux-namespace"},{"kind",kind},{"workspace",workspace.native()},
            {"task_id",task.empty()?Json(nullptr):Json(task)},{"owner",{{"pid",getpid()},{"identity",current_process_identity()},{"uid",static_cast<unsigned>(geteuid())}}},
            {"phase","preparing"},{"resources",Json::array()}};
        write_record(directory.value,record);if(fsync(registry.fd()))posix_fail("persist new resource journal");
    }
    void verify() const {require(!finished,"resource journal already finished");registry.verify();verify_entry(registry.fd(),id,directory.value);verify_entry(directory.value,"lease",lock.value);}
};
SandboxResourceLease::SandboxResourceLease(const std::string& id,const std::string& kind,const fs::path& workspace,const std::string& task):impl_(std::make_unique<Impl>(id,kind,workspace,task)){}
SandboxResourceLease::~SandboxResourceLease()=default;
fs::path sandbox_resource_registry_path() {return registry_path();}
void SandboxResourceLease::expect(const std::string& kind,const fs::path& path) {
    auto& value=*impl_;value.verify();resource_path(kind,path,value.id);
    require(value.record["phase"]=="preparing","resources cannot be added after preparation");
    for(const auto& existing:value.record["resources"])require(existing["kind"]!=kind,"resource kind already registered");
    require(value.record["resources"].size()<2,"resource inventory exceeds limit");
    value.record["resources"].push_back({{"kind",kind},{"path",path.native()},{"captured",false},{"chain",Json::array()}});
    write_record(value.directory.value,value.record);
}
void SandboxResourceLease::track(const std::string& kind,const fs::path& path) {
    auto& value=*impl_;value.verify();resource_path(kind,path,value.id);
    require(value.record["phase"]=="preparing","resources cannot be captured after preparation");
    for(auto& item:value.record["resources"])if(item["kind"]==kind){
        require(item["path"]==path.native()&&!item["captured"].get<bool>(),"resource path differs from creation intent");
        Anchor anchor(path);require(metadata(anchor.fd()).st_uid==geteuid(),"resource directory is not owned by caller");
        if(kind=="staging")require(!identity(anchor.fd())["birth_seconds"].is_null(),"staging filesystem must supply a directory birth identity");
        item["chain"]=anchor.chain();item["captured"]=true;anchor.verify();write_record(value.directory.value,value.record);return;
    }
    fail("resource has no creation intent");
}
void SandboxResourceLease::record_job(const std::string&) {fail("Windows Job names are unsupported on Linux");}
void SandboxResourceLease::mark_ready() {
    auto& value=*impl_;value.verify();require(value.record["resources"].size()==2,"resource inventory incomplete");
    for(const auto& item:value.record["resources"])require(item["captured"].get<bool>(),"resource identity was not captured");
    value.record["phase"]="ready";write_record(value.directory.value,value.record);
}
void SandboxResourceLease::finish() {
    auto& value=*impl_;if(value.finished)return;value.verify();
    for(const auto& item:value.record["resources"]){auto view=inspect_resource(item,value.record["owner"]);require(view.anchor->missing,"cannot finish while resources remain");}
    // Own lock is already held; borrow duplicate descriptors without releasing it.
    OpenRecord opened(value.root,value.id,false);opened.lock=Fd(fcntl(value.lock.value,F_DUPFD_CLOEXEC,3));
    if(opened.lock.value<0)posix_fail("duplicate owned lease");opened.locked=true;
    remove_journal(opened);value.finished=true;
}
Json list_sandbox_resources(const std::string& after,unsigned limit) {
    if(!after.empty())valid_id(after);require(limit>=1&&limit<=100,"resource page limit must be 1..100");
    const auto root=registry_path();Json result={{"format",2},{"registry",root.native()},{"items",Json::array()},{"next_after",nullptr}};
    Anchor registry(root,false,true);if(registry.missing)return result;private_directory(registry.fd());registry.verify();
    auto names=entries(registry.fd(),10000);for(const auto& name:names)valid_id(name);
    names.erase(std::remove_if(names.begin(),names.end(),[&](const auto& name){return name<=after;}),names.end());
    constexpr std::size_t page_bytes=512*1024;
    std::size_t bytes=0;
    std::string last;
    for(const auto& id:names){
        if(result["items"].size()==limit){result["next_after"]=last;break;}
        Json item;
        try{OpenRecord opened(root,id,true);if(opened.missing())continue;auto record=parse_record(read_small(opened.directory.value,"record.json",record_limit,true),id);item=inspect(opened,record).report;}
        catch(const std::exception& error){item={{"id",id},{"state","invalid"},{"reason",error.what()},{"resources",Json::array()}};}
        const auto size=item.dump().size();require(size<=page_bytes,"resource item exceeds page byte limit");
        if(size>page_bytes-bytes){result["next_after"]=last;break;}
        bytes+=size;last=id;result["items"].push_back(std::move(item));
    }
    registry.verify();return result;
}
Json reclaim_sandbox_resource(const std::string& id,bool dry_run,const std::atomic_bool& cancelled) {
    valid_id(id);Json result={{"format",2},{"id",id},{"dry_run",dry_run},{"state","blocked"},{"reason",""},{"resources",Json::array()}};
    if(cancelled.load()){result["state"]="cancelled";result["reason"]="cancelled_before_inspection";return result;}
    bool started=false;
    try{
        OpenRecord opened(registry_path(),id,true);
        if(opened.missing()){result["state"]="absent";result["reason"]="no_registered_resource";return result;}
        auto record=parse_record(read_small(opened.directory.value,"record.json",record_limit,true),id);
        auto view=inspect(opened,record);result.update(view.report);result["dry_run"]=dry_run;
        if(result["state"]!="reclaimable"||dry_run)return result;
        cancelled_check(cancelled);opened.verify();
        // Revalidate the immutable record under the same lease before mutation.
        require(parse_record(read_small(opened.directory.value,"record.json",record_limit,true),id)==record,"resource record changed before cleanup");
        record["phase"]="reclaiming";write_record(opened.directory.value,record);result["phase"]="reclaiming";started=true;
        // Verify all roots before deleting anything. Each root remains anchored.
        for(auto& item:view.resources)item.anchor->verify();
        for(auto& item:view.resources){
            cancelled_check(cancelled);auto& anchor=*item.anchor;if(anchor.missing)continue;
            anchor.verify();const auto parent=anchor.fds[anchor.fds.size()-2].value;const auto name=anchor.names.back();
            if(item.report["kind"]=="cgroup"){
                const auto events=read_small(anchor.fd(),"cgroup.events",4096,false);
                require(events.find("populated 0\n")!=std::string::npos&&events.find("populated 1")==std::string::npos,"cgroup became populated");
                if(unlinkat(parent,name.c_str(),AT_REMOVEDIR))posix_fail("remove empty owned cgroup");
            }else{std::size_t budget=1000000;const auto expected_root=identity(anchor.fd());
                remove_tree(parent,name,expected_root["mount"].get<std::uint64_t>(),0,budget,cancelled,&expected_root);}
        }
        cancelled_check(cancelled);opened.verify();
        for(const auto& item:record["resources"]){auto absence=inspect_resource(item,record["owner"]);require(absence.anchor->missing,"resource remains after cleanup");}
        remove_journal(opened);result["state"]="reclaimed";result["reason"]="registered_resources_removed";
        for(auto& item:result["resources"])item["state"]="missing";return result;
    }catch(const std::exception& error){result["state"]=cancelled.load()?"cancelled":started?"incomplete":"blocked";result["reason"]=error.what();return result;}
}
}
#endif
