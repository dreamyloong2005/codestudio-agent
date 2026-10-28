#include "codestudio/sandbox_resources.hpp"
#ifdef _WIN32
#include "codestudio/process_observation.hpp"
#include "codestudio/store.hpp"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace codestudio {
namespace {
namespace fs=std::filesystem;
constexpr DWORD shares=FILE_SHARE_READ|FILE_SHARE_WRITE;
constexpr std::size_t record_limit=128*1024;
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
[[noreturn]] void failure(const char* operation) {throw std::system_error(GetLastError(),std::system_category(),operation);}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v=INVALID_HANDLE_VALUE):value(v){}
    ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(Handle&& other) noexcept:value(other.value){other.value=INVALID_HANDLE_VALUE;}
    Handle& operator=(Handle&& other) noexcept {if(this!=&other){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=other.value;other.value=INVALID_HANDLE_VALUE;}return *this;}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    bool valid() const{return value && value!=INVALID_HANDLE_VALUE;}
};
struct LocalMemory {void* value=nullptr;~LocalMemory(){if(value)LocalFree(value);}};
struct TaskMemory {PWSTR value=nullptr;~TaskMemory(){if(value)CoTaskMemFree(value);}};
std::string text(const fs::path& p){const auto s=p.generic_u8string();return {s.begin(),s.end()};}
fs::path path(const std::string& s){auto value=fs::path(std::u8string(s.begin(),s.end()));value.make_preferred();return value;}
bool identifier(const std::string& id){return id.size()==32 && std::ranges::all_of(id,[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
void cancelled(const std::atomic_bool& value){if(value.load())throw std::runtime_error("Sandbox resource cleanup cancelled");}
std::wstring owner_sid(){
    HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))failure("Open sandbox registry owner");Handle token(raw);
    DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);require(size>0,"Cannot measure sandbox registry owner");
    std::vector<unsigned char> value(size);if(!GetTokenInformation(token.value,TokenUser,value.data(),size,&size))failure("Read sandbox registry owner");
    LocalMemory sid;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(value.data())->User.Sid,reinterpret_cast<LPWSTR*>(&sid.value)))failure("Encode sandbox registry owner");
    return static_cast<wchar_t*>(sid.value);
}
std::wstring private_acl(){const auto owner=owner_sid();return L"O:"+owner+L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;"+owner+L")";}
void make_private(const fs::path& p){
    LocalMemory descriptor;const auto acl=private_acl();
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value),nullptr))failure("Build sandbox registry ACL");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor.value,FALSE};
    if(!CreateDirectoryW(p.c_str(),&attributes))failure("Create private sandbox registry directory");
}
void private_directory(HANDLE handle){
    LocalMemory descriptor;PSID owner=nullptr;PACL dacl=nullptr;
    const auto error=GetSecurityInfo(handle,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&dacl,nullptr,reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value));
    if(error!=ERROR_SUCCESS)throw std::system_error(error,std::system_category(),"Inspect sandbox registry ACL");
    LocalMemory expected;const auto sid=owner_sid();
    if(!ConvertStringSidToSidW(sid.c_str(),&expected.value))failure("Parse sandbox registry owner SID");
    require(owner && EqualSid(owner,expected.value),"Sandbox registry belongs to another user");
    SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision=0;
    require(GetSecurityDescriptorControl(descriptor.value,&control,&revision) && (control&SE_DACL_PROTECTED) && dacl && dacl->AceCount==2,"Sandbox registry must retain a private protected ACL");
    bool user=false,system=false;
    for(DWORD i=0;i<dacl->AceCount;++i){
        void* entry=nullptr;require(GetAce(dacl,i,&entry),"Cannot inspect sandbox registry ACE");
        const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(entry);
        require(ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE && ace->Mask==FILE_ALL_ACCESS,"Sandbox registry has unexpected permissions");
        auto* identity=const_cast<DWORD*>(&ace->SidStart);
        if(EqualSid(identity,expected.value)){require(!user,"Repeated sandbox registry owner ACE");user=true;}
        else {require(IsWellKnownSid(identity,WinLocalSystemSid) && !system,"Sandbox registry grants another principal");system=true;}
    }
    require(user && system,"Sandbox registry lacks its owner permissions");
}
BY_HANDLE_FILE_INFORMATION information(HANDLE handle){BY_HANDLE_FILE_INFORMATION result{};if(!GetFileInformationByHandle(handle,&result))failure("Inspect sandbox resource identity");return result;}
Json identity(HANDLE handle){const auto v=information(handle);return {{"volume",v.dwVolumeSerialNumber},{"index_high",v.nFileIndexHigh},{"index_low",v.nFileIndexLow},{"created_high",v.ftCreationTime.dwHighDateTime},{"created_low",v.ftCreationTime.dwLowDateTime}};}
bool is_missing(DWORD error){return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND;}
bool same_path(const fs::path& a,const fs::path& b){auto first=a,second=b;first.make_preferred();second.make_preferred();return CompareStringOrdinal(first.c_str(),-1,second.c_str(),-1,TRUE)==CSTR_EQUAL;}
void spelling(const fs::path& p){
    require(p.is_absolute() && !p.native().starts_with(L"\\\\") && p.native().find(L'\0')==std::wstring::npos && p.lexically_normal()==p,"Sandbox resource path must be a normalized local absolute path");
    require(GetDriveTypeW(p.root_path().c_str())==DRIVE_FIXED,"Sandbox resources require a local fixed drive");
}
fs::path native_path(HANDLE h){
    std::wstring observed(32768,L'\0');const auto n=GetFinalPathNameByHandleW(h,observed.data(),static_cast<DWORD>(observed.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!n || n>=observed.size())failure("Resolve sandbox resource handle");observed.resize(n);
    require(observed.starts_with(L"\\\\?\\"),"Sandbox resource path has an unexpected native name");observed.erase(0,4);
    return fs::path(observed);
}
void exact_path(HANDLE h,const fs::path& p){
    require(same_path(native_path(h),p),"Sandbox resource path aliases are forbidden");
}
struct Chain {std::vector<Handle> handles;bool exists=false;};
Chain anchor(const fs::path& p,bool missing=false,DWORD leaf_access=FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES){
    spelling(p);Chain result;fs::path current=p.root_path();std::vector<fs::path> components{current};
    for(const auto& part:p.relative_path()){require(part!=L"." && part!=L"..","Sandbox resource path must be normalized");current/=part;components.push_back(current);}
    for(std::size_t i=0;i<components.size();++i){
        auto h=Handle(CreateFileW(components[i].c_str(),READ_CONTROL|(i+1==components.size()?leaf_access:FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES),shares,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h.valid()){const auto error=GetLastError();if(missing && is_missing(error))return result;SetLastError(error);failure("Open sandbox resource anchor");}
        const auto v=information(h.value);require((v.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(v.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Sandbox resource directory is linked or not a directory");
        exact_path(h.value,components[i]);result.handles.push_back(std::move(h));
    }
    result.exists=true;return result;
}
fs::path local_app_data(){TaskMemory value;const auto error=SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&value.value);if(FAILED(error))throw std::system_error(error,std::system_category(),"Locate sandbox profile directory");return fs::path(value.value);}
fs::path default_registry_parent(){
    // Packaged hosts can redirect LocalAppData even when child processes have
    // no package identity. Keep this per-user journal outside that mapping so
    // its protected path is stable before creation and across launch hosts.
    TaskMemory value;
    const auto error=SHGetKnownFolderPath(FOLDERID_Profile,KF_FLAG_DONT_VERIFY,nullptr,&value.value);
    if(FAILED(error))throw std::system_error(error,std::system_category(),"Locate sandbox registry directory");
    return fs::path(value.value)/L".codestudio-agent";
}
fs::path registry_path(){
    std::array<wchar_t,32768> value{};const auto count=GetEnvironmentVariableW(L"CODESTUDIO_SANDBOX_REGISTRY",value.data(),static_cast<DWORD>(value.size()));
    if(count){require(count<value.size(),"Sandbox registry path is too long");fs::path p(std::wstring(value.data(),count));spelling(p);return p;}
    return default_registry_parent()/L"sandbox-resources";
}
Chain registry(bool create){
    const auto p=registry_path();auto initial=anchor(p,true);if(initial.exists){private_directory(initial.handles.back().value);return initial;}
    if(!create)return initial;
    // Existing ancestors are pinned throughout creation; only the default
    // product directory or the final explicitly selected directory is created.
    if(!fs::exists(p.parent_path())){
        require(same_path(p.parent_path(),default_registry_parent()),"Sandbox registry parent must already exist");
        auto parent=anchor(p.parent_path().parent_path());make_private(p.parent_path());
    }
    auto parent=anchor(p.parent_path());
    LocalMemory descriptor;const auto acl=private_acl();if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value),nullptr))failure("Build sandbox registry ACL");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor.value,FALSE};
    if(!CreateDirectoryW(p.c_str(),&attributes) && GetLastError()!=ERROR_ALREADY_EXISTS)failure("Create sandbox registry root");
    auto result=anchor(p);private_directory(result.handles.back().value);return result;
}
std::vector<std::string> children(const fs::path& p){
    std::vector<std::string> result;WIN32_FIND_DATAW data{};const auto search=FindFirstFileW((p/L"*").c_str(),&data);
    if(search==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return result;failure("Enumerate sandbox registry");}
    DWORD last=ERROR_SUCCESS;
    do {const std::wstring name=data.cFileName;if(name!=L"." && name!=L".."){result.push_back(text(fs::path(name)));if(result.size()>10000){FindClose(search);throw std::runtime_error("Sandbox registry enumeration exceeds bound");}}}while(FindNextFileW(search,&data));
    last=GetLastError();FindClose(search);require(last==ERROR_NO_MORE_FILES,"Sandbox registry enumeration failed");return result;
}
Json read_record(const fs::path& p){
    Handle file(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));if(!file.valid())failure("Open sandbox ownership record");
    const auto v=information(file.value);require(!(v.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) && v.nNumberOfLinks==1 && !v.nFileSizeHigh && v.nFileSizeLow>0 && v.nFileSizeLow<=record_limit,"Sandbox record must be a bounded single-link regular file");
    std::string data(v.nFileSizeLow,'\0');DWORD count=0;if(!ReadFile(file.value,data.data(),static_cast<DWORD>(data.size()),&count,nullptr) || count!=data.size())failure("Read sandbox ownership record");
    std::vector<std::set<std::string>> keys;bool duplicate=false;
    auto parsed=Json::parse(data,[&](int,Json::parse_event_t event,Json& value){if(event==Json::parse_event_t::object_start)keys.emplace_back();else if(event==Json::parse_event_t::object_end)keys.pop_back();else if(event==Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)duplicate=true;return true;});
    require(!duplicate,"Sandbox record contains duplicate properties");return parsed;
}
void write_record(const fs::path& entry,const Json& record){
    const auto data=record.dump();require(data.size()<=record_limit,"Sandbox record exceeds bound");
    const auto temporary=entry/path(".record-next-"+new_id());
    Handle output(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));if(!output.valid())failure("Create sandbox ownership snapshot");
    DWORD count=0;if(!WriteFile(output.value,data.data(),static_cast<DWORD>(data.size()),&count,nullptr) || count!=data.size() || !FlushFileBuffers(output.value))failure("Persist sandbox ownership snapshot");
    output=Handle();
    if(!MoveFileExW(temporary.c_str(),(entry/L"record.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))failure("Publish sandbox ownership snapshot");
}
void validate_record(const Json& r,const std::string& id){
    require(r.is_object() && r.size()==11 && r.contains("format") && r["format"].is_number_integer() && r["format"]==2 && r.value("id",Json())==id &&
        r.value("backend",Json())=="windows-lpac" && r.contains("kind") && r["kind"].is_string() &&
        (r["kind"]=="process"||r["kind"]=="package"||r["kind"]=="build") && r.contains("workspace") && r["workspace"].is_string() &&
        r.contains("task_id") && (r["task_id"].is_null()||r["task_id"].is_string()) &&
        r.contains("owner") && r["owner"].is_object() && r["owner"].size()==2 && r["owner"].contains("pid") && r["owner"]["pid"].is_number_integer() &&
        r["owner"].contains("identity") && r["owner"]["identity"].is_string() && r.contains("phase") &&
        (r["phase"]=="preparing"||r["phase"]=="ready") && r.contains("resources") && r["resources"].is_array() && r["resources"].size()<=2 &&
        r.contains("jobs") && r["jobs"].is_array() && r["jobs"].size()<=256 && r.contains("locations") && r["locations"].is_object() && r["locations"].size()==2,
        "Invalid Windows sandbox ownership record");
    require(r["locations"].contains("temporary") && r["locations"]["temporary"].is_string() && r["locations"].contains("profiles") && r["locations"]["profiles"].is_string(),"Invalid sandbox resource locations");
    require(same_path(path(r["locations"]["temporary"].get<std::string>()),fs::canonical(fs::temp_directory_path())) &&
        same_path(path(r["locations"]["profiles"].get<std::string>()),local_app_data()/L"Packages"),"Sandbox resource location no longer matches this user context");
    const auto base="codestudio-sandbox-"+id;std::set<std::string> kinds;
    for(const auto& item:r["resources"]){
        require(item.is_object() && item.size()==4 && item.contains("kind") && item["kind"].is_string() && item.contains("path") && item["path"].is_string() && item.contains("identity") && item.contains("parent_identity"),"Invalid sandbox resource entry");
        const auto kind=item["kind"].get<std::string>();require((kind=="staging"||kind=="profile") && kinds.insert(kind).second,"Invalid sandbox resource kind");
        const auto parent=path(r["locations"][kind=="staging"?"temporary":"profiles"].get<std::string>());
        require(same_path(path(item["path"].get<std::string>()),parent/path(base)),"Sandbox resource path differs from its registered identity");
        const auto native_identity=[](const Json& value){
            if(!value.is_object() || value.size()!=5)return false;
            for(const auto* key:{"volume","index_high","index_low","created_high","created_low"})
                if(!value.contains(key)||!value[key].is_number_integer()||value[key]<0||value[key]>0xffffffffULL)return false;
            return true;
        };
        require(item["identity"].is_null()||native_identity(item["identity"]),"Invalid sandbox resource identity");
        require(native_identity(item["parent_identity"]),"Missing sandbox resource parent identity");
    }
    std::set<std::string> jobs;const auto prefix="Global\\CodeStudio-Agent-Sandbox-"+id+"-";
    for(const auto& job:r["jobs"]){require(job.is_string(),"Invalid sandbox Job name");const auto value=job.get<std::string>();require(value.starts_with(prefix)&&identifier(value.substr(prefix.size()))&&jobs.insert(value).second,"Unexpected sandbox Job identity");}
}
Handle lease(const fs::path& entry,bool create){
    Handle h(CreateFileW((entry/L"lease.lock").c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,create?CREATE_NEW:OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(h.valid()){const auto v=information(h.value);require(!(v.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&v.nNumberOfLinks==1&&v.nFileSizeHigh==0&&v.nFileSizeLow==0,"Invalid sandbox lease file");}
    return h;
}
void dispose(HANDLE h){FILE_DISPOSITION_INFO info{TRUE};if(!SetFileInformationByHandle(h,FileDispositionInfo,&info,sizeof(info)))failure("Remove owned sandbox resource");}
void remove_tree(const fs::path& p,HANDLE pinned,unsigned depth,std::size_t& budget,const std::atomic_bool& stop){
    cancelled(stop);require(depth<=64 && budget>0,"Sandbox cleanup traversal exceeds bound");--budget;
    const auto v=information(pinned);
    if((v.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(v.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
        for(const auto& name:children(p)){
            cancelled(stop);const auto child=p/path(name);
            Handle h(CreateFileW(child.c_str(),DELETE|FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|FILE_LIST_DIRECTORY,shares,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
            if(!h.valid())failure("Open owned sandbox cleanup entry");remove_tree(child,h.value,depth+1,budget,stop);
        }
    }
    if((v.dwFileAttributes&FILE_ATTRIBUTE_READONLY) && v.nNumberOfLinks==1){FILE_BASIC_INFO basic{};basic.FileAttributes=v.dwFileAttributes&~FILE_ATTRIBUTE_READONLY;if(!basic.FileAttributes)basic.FileAttributes=FILE_ATTRIBUTE_NORMAL;if(!SetFileInformationByHandle(pinned,FileBasicInfo,&basic,sizeof(basic)))failure("Clear owned sandbox readonly flag");}
    cancelled(stop);dispose(pinned);
}
void remove_journal(const fs::path& entry,Chain& pins,Handle& lock){
    // Only files created by this journal are removed. Unknown entries remain.
    const auto entries=children(entry);
    for(const auto& name:entries)require(name=="record.json" || name=="lease.lock" || (name.starts_with(".record-next-") && identifier(name.substr(13))),"Sandbox journal contains unknown files");
    // Normal readers pin without DELETE so they can observe active leases.
    // Upgrade before changing any journal files; contention leaves the intact
    // record available for a later retry.
    const auto expected=identity(pins.handles.back().value);pins.handles.pop_back();
    Handle directory(CreateFileW(entry.c_str(),DELETE|FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,shares,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(!directory.valid())failure("Open completed sandbox journal directory");
    const auto state=information(directory.value);require((state.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(state.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&identity(directory.value)==expected,"Sandbox journal directory identity changed");
    for(const auto& name:entries)if(name!="lease.lock"){
        Handle file(CreateFileW((entry/path(name)).c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(!file.valid())failure("Open completed sandbox journal file");
        const auto v=information(file.value);require(!(v.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&v.nNumberOfLinks==1,"Sandbox journal file identity is unsafe");dispose(file.value);
    }
    dispose(lock.value);lock=Handle();dispose(directory.value);
}
struct Inspection {
    Json record,view;
    Chain directory;
    Handle lock;
    std::vector<Chain> resources;
    std::vector<Handle> jobs;
};
Inspection inspect(const fs::path& root,const std::string& id){
    Inspection state;state.view={{"id",id},{"state","invalid"},{"reason","invalid_record"},{"resources",Json::array()}};
    const auto entry=root/path(id);
    state.directory=anchor(entry,true);
    if(!state.directory.exists){state.view["state"]="absent";state.view["reason"]="not_registered";return state;}
    private_directory(state.directory.handles.back().value);
    state.record=read_record(entry/L"record.json");validate_record(state.record,id);
    for(const auto* key:{"kind","workspace","task_id","owner","phase","resources"})state.view[key]=state.record[key];
    state.lock=lease(entry,false);
    if(!state.lock.valid()){
        if(GetLastError()==ERROR_SHARING_VIOLATION){state.view["state"]="active";state.view["reason"]="lease_held";return state;}
        state.view["state"]="blocked";state.view["reason"]="lease_unavailable";return state;
    }
    const auto observed=observe_process_identity(state.record["owner"]["pid"].get<std::int64_t>(),state.record["owner"]["identity"].get<std::string>());state.view["owner_observation"]=observed;
    if(observed!="not_running" && observed!="identity_mismatch"){
        state.view["state"]=observed=="matching_process"?"active":"blocked";state.view["reason"]=observed=="matching_process"?"owner_running":"owner_unavailable";return state;
    }
    for(const auto& value:state.record["jobs"]){
        const auto name=path(value.get<std::string>()).native();Handle job(OpenJobObjectW(JOB_OBJECT_QUERY,FALSE,name.c_str()));
        if(!job.valid()){if(GetLastError()==ERROR_FILE_NOT_FOUND)continue;state.view["state"]="blocked";state.view["reason"]="job_unavailable";return state;}
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if(!QueryInformationJobObject(job.value,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr)){state.view["state"]="blocked";state.view["reason"]="job_unavailable";return state;}
        if(accounting.ActiveProcesses){state.view["state"]="blocked";state.view["reason"]="job_populated";return state;}state.jobs.push_back(std::move(job));
    }
    for(std::size_t index=0;index<state.record["resources"].size();++index){
        const auto& item=state.record["resources"][index];const auto p=path(item["path"].get<std::string>());
        auto parent=anchor(p.parent_path());require(identity(parent.handles.back().value)==item["parent_identity"],"Sandbox resource parent identity changed");
        auto resource=anchor(p,true,DELETE|FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|FILE_LIST_DIRECTORY);
        if(resource.exists){
            if(item["identity"].is_null()){state.view["state"]="blocked";state.view["reason"]="uncaptured_resource";return state;}
            require(identity(resource.handles.back().value)==item["identity"],"Sandbox resource directory identity changed");
        }
        state.view["resources"][index]["observation"]=resource.exists?"matching":"absent";
        state.resources.push_back(std::move(resource));
    }
    state.view["state"]="reclaimable";state.view["reason"]="owner_exited_resources_verified";return state;
}
}

struct SandboxResourceLease::Impl {
    fs::path root,entry;Chain registry_pins,entry_pins;Handle lock;Json record;
    void save(){write_record(entry,record);}
};
SandboxResourceLease::SandboxResourceLease(const std::string& id,const std::string& kind,const fs::path& workspace,const std::string& task_id):impl_(std::make_unique<Impl>()){
    require(identifier(id)&&(kind=="process"||kind=="package"||kind=="build"),"Invalid sandbox resource lease identity");
    require(task_id.size()<=128 && task_id.find('\0')==std::string::npos,"Invalid sandbox task identity");
    auto& v=*impl_;v.registry_pins=registry(true);v.root=registry_path();v.entry=v.root/path(id);make_private(v.entry);v.entry_pins=anchor(v.entry);v.lock=lease(v.entry,true);if(!v.lock.valid())failure("Create sandbox ownership lease");
    const auto temporary=fs::canonical(fs::temp_directory_path());const auto profiles=local_app_data()/L"Packages";
    v.record={{"format",2},{"id",id},{"backend","windows-lpac"},{"kind",kind},{"workspace",text(workspace)},{"task_id",task_id.empty()?Json():Json(task_id)},
        {"owner",{{"pid",GetCurrentProcessId()},{"identity",current_process_identity()}}},{"phase","preparing"},{"resources",Json::array()},{"jobs",Json::array()},
        {"locations",{{"temporary",text(temporary)},{"profiles",text(profiles)}}}};v.save();
}
SandboxResourceLease::~SandboxResourceLease()=default;
fs::path sandbox_resource_registry_path(){return registry_path();}
void SandboxResourceLease::expect(const std::string& kind,const fs::path& supplied){
    auto& v=*impl_;require(kind=="staging"||kind=="profile","Unsupported Windows sandbox resource kind");
    for(const auto& item:v.record["resources"])require(item["kind"]!=kind,"Sandbox resource was already registered");
    const auto expected=path(v.record["locations"][kind=="staging"?"temporary":"profiles"].get<std::string>())/path("codestudio-sandbox-"+v.record["id"].get<std::string>());
    require(same_path(supplied,expected),"Sandbox path differs from its planned identity");auto parent=anchor(expected.parent_path());
    auto existing=anchor(expected,true);require(!existing.exists,"Sandbox resource intent requires an absent path");
    v.record["resources"].push_back({{"kind",kind},{"path",text(expected)},{"identity",nullptr},{"parent_identity",identity(parent.handles.back().value)}});v.save();
}
void SandboxResourceLease::track(const std::string& kind,const fs::path& supplied){
    auto& v=*impl_;for(auto& item:v.record["resources"])if(item["kind"]==kind){require(item["identity"].is_null()&&same_path(supplied,path(item["path"].get<std::string>())),"Sandbox resource capture differs from intent");
        auto parent=anchor(supplied.parent_path());require(identity(parent.handles.back().value)==item["parent_identity"],"Sandbox resource parent changed before capture");auto pins=anchor(supplied);item["identity"]=identity(pins.handles.back().value);v.save();return;}
    throw std::runtime_error("Sandbox resource capture has no intent");
}
void SandboxResourceLease::record_job(const std::string& name){auto& v=*impl_;require(v.record["jobs"].size()<256,"Sandbox Job inventory exceeds bound");v.record["jobs"].push_back(name);validate_record(v.record,v.record["id"].get<std::string>());v.save();}
void SandboxResourceLease::mark_ready(){auto& v=*impl_;for(const auto& r:v.record["resources"])require(!r["identity"].is_null(),"Sandbox resource was not captured before activation");v.record["phase"]="ready";v.save();}
void SandboxResourceLease::finish(){auto& v=*impl_;if(!v.lock.valid())return;for(const auto& r:v.record["resources"]){auto found=anchor(path(r["path"].get<std::string>()),true);require(!found.exists,"Sandbox resources remain after cleanup");}remove_journal(v.entry,v.entry_pins,v.lock);}
Json list_sandbox_resources(const std::string& after,unsigned limit){
    require((after.empty()||identifier(after))&&limit>=1&&limit<=100,"Invalid sandbox resource page");const auto root=registry_path();Json out={{"format",2},{"registry",text(root)},{"items",Json::array()},{"next_after",nullptr}};
    auto pins=registry(false);if(!pins.exists)return out;auto names=children(root);std::ranges::sort(names);std::vector<std::string> selected;
    for(const auto& name:names)if(identifier(name)&&name>after)selected.push_back(name);
    constexpr std::size_t page_bytes=512*1024;std::size_t used=0;
    for(const auto& id:selected){
        if(out["items"].size()>=limit){out["next_after"]=out["items"].back()["id"];break;}
        Json item;
        try{auto inspected=inspect(root,id);if(inspected.view["state"]=="absent")continue;item=std::move(inspected.view);}
        catch(const std::exception&){item={{"id",id},{"state","invalid"},{"reason","record_or_resource_identity_invalid"},{"resources",Json::array()}};}
        const auto bytes=item.dump().size()+(out["items"].empty()?0:1);
        require(bytes<=page_bytes,"Sandbox resource item exceeds page byte bound");
        if(bytes>page_bytes-used){out["next_after"]=out["items"].back()["id"];break;}
        used+=bytes;out["items"].push_back(std::move(item));
    }
    return out;
}
Json reclaim_sandbox_resource(const std::string& id,bool dry_run,const std::atomic_bool& stop){
    require(identifier(id),"Invalid sandbox resource id");Json out={{"format",2},{"id",id},{"dry_run",dry_run},{"state","blocked"},{"reason","record_or_resource_identity_invalid"},{"resources",Json::array()}};
    if(stop.load()){out["state"]="cancelled";out["reason"]="cancel_requested";return out;}
    try{
        const auto root=registry_path();auto root_pins=registry(false);if(!root_pins.exists){out["state"]="absent";out["reason"]="not_registered";return out;}
        auto state=inspect(root,id);out.update(state.view);out["format"]=2;out["dry_run"]=dry_run;
        if(state.view["state"]!="reclaimable" || dry_run)return out;
        cancelled(stop);
        for(std::size_t index=0;index<state.resources.size();++index){
            auto& pins=state.resources[index];const auto& resource=state.record["resources"][index];const auto p=path(resource["path"].get<std::string>());std::size_t budget=1000000;
            if(!pins.exists && (resource["kind"]!="profile" || resource["identity"].is_null()))continue;
            if(resource["kind"]=="profile"){
                // Remove each child by its pinned handle, never give untrusted
                // descendants to the recursive profile API.
                if(pins.exists){
                    for(const auto& name:children(p)){cancelled(stop);Handle child(CreateFileW((p/path(name)).c_str(),DELETE|FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|FILE_LIST_DIRECTORY,shares,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));if(!child.valid())failure("Open owned profile child");remove_tree(p/path(name),child.value,1,budget,stop);}
                    require(children(p).empty(),"Sandbox profile did not become empty");cancelled(stop);pins.handles.pop_back();
                }
                // Filesystem removal and the Windows profile registration are
                // separate OS effects. Retry the API even when a previous
                // interrupted attempt already removed the captured directory.
                const auto name=path("codestudio-sandbox-"+id).native();const auto result=DeleteAppContainerProfile(name.c_str());
                const bool missing=result==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)||result==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)||result==HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                if(FAILED(result)&&!missing)throw std::system_error(result,std::system_category(),"Remove owned sandbox profile");
            }else{remove_tree(p,pins.handles.back().value,0,budget,stop);pins.handles.pop_back();}
            pins.exists=false;
        }
        for(const auto& resource:state.record["resources"]){auto remaining=anchor(path(resource["path"].get<std::string>()),true);require(!remaining.exists,"Sandbox resource cleanup remains incomplete");}
        cancelled(stop);remove_journal(root/path(id),state.directory,state.lock);out["state"]="reclaimed";out["reason"]="owned_resources_removed";return out;
    }catch(const std::exception&){if(stop.load()){out["state"]="cancelled";out["reason"]="cancel_requested";}else if(out["state"]=="reclaimable"){out["state"]="incomplete";out["reason"]="cleanup_incomplete";}return out;}
}
}
#endif
