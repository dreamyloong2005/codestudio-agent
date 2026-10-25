#include "codestudio/process_package.hpp"
#include "codestudio/build_workspace.hpp"
#include "codestudio/digest.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace codestudio {
namespace {
namespace fs=std::filesystem;
constexpr std::size_t byte_limit=64*1024*1024;
void require(bool condition,const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
std::string utf8(const fs::path& path) { const auto text=path.generic_u8string(); return {text.begin(),text.end()}; }
fs::path path_of(const std::string& text) { return fs::path(std::u8string(text.begin(),text.end())); }
std::string relative_name(const fs::path& path) {
    const auto text=utf8(path);
    require(!text.empty() && text.size()<=240 && !path.has_root_path() && text.back()!='/' &&
            text.find_first_of("\\:")==std::string::npos && utf8(path.lexically_normal())==text,
            "Invalid process package relative path");
    for (const auto& part:path) {
        const auto component=utf8(part);
        require(!component.empty() && component!="." && component!=".." && component.back()!='.' &&
                std::ranges::all_of(component,[](unsigned char c) {
                    return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.';
                }),"Invalid process package path component");
        auto base=component.substr(0,component.find('.'));
        std::ranges::transform(base,base.begin(),[](unsigned char c) {return c>='A' && c<='Z' ? c+32 : c;});
        require(base!="con" && base!="prn" && base!="aux" && base!="nul" &&
            !(base.size()==4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3]>='1' && base[3]<='9'),
            "Reserved process package path");
    }
    return text;
}
void check(const std::atomic_bool& cancelled) {
    if (cancelled.load()) throw std::runtime_error("Process package preparation cancelled");
}
}
struct ProcessPackage::Impl {
    struct File { fs::path path; std::string digest; std::size_t size; bool executable; };
    fs::path source,entrypoint;
    std::vector<File> files;
    std::vector<std::string> arguments;
};
ProcessPackage::ProcessPackage(const fs::path& source,const Json& files,const fs::path& entrypoint,
                               const std::vector<std::string>& arguments) {
    auto value=std::make_shared<Impl>();
    require(source.is_absolute() && source.lexically_normal()==source,"Process package source must be absolute and normalized");
    value->source=source; value->entrypoint=entrypoint;
    const auto main=relative_name(entrypoint);
    require(files.is_array() && !files.empty() && files.size()<=128,"Invalid process package inventory size");
    std::set<std::string> names,folded;
    std::size_t total=0;
    bool executable=false;
    for (const auto& record:files) {
        require(record.is_object() && record.size()==4 && record.contains("path") && record["path"].is_string() &&
                record.contains("sha256") && record["sha256"].is_string() && record.contains("size") &&
                record["size"].is_number_integer() && record["size"]>=0 && record["size"]<=byte_limit &&
                record.contains("executable") && record["executable"].is_boolean(),"Invalid process package file record");
        const auto raw=record["path"].get<std::string>();
        const auto path=path_of(raw);
        require(raw==utf8(path),"Noncanonical process package path");
        const auto name=relative_name(path);
        auto lower=name; std::ranges::transform(lower,lower.begin(),[](unsigned char c){return c>='A'&&c<='Z'?c+32:c;});
        require(names.insert(name).second && folded.insert(lower).second,"Duplicate process package path");
        const auto hash=record["sha256"].get<std::string>();
        require(hash.size()==64 && std::ranges::all_of(hash,[](char c){return (c>='a'&&c<='f')||(c>='0'&&c<='9');}),
                "Invalid process package digest");
        const auto size=record["size"].get<std::size_t>();
        require(size<=byte_limit-total,"Process package exceeds byte limit"); total+=size;
        const auto mode=record["executable"].get<bool>();
        if (name==main) executable=mode && size>0;
        value->files.push_back({path,hash,size,mode});
    }
    require(executable,"Process package entrypoint must be a declared executable");
    std::map<std::string,std::string> directories;
    for (const auto& file:value->files) for (auto parent=file.path.parent_path();!parent.empty();parent=parent.parent_path()) {
        const auto original=utf8(parent);
        auto name=original;std::ranges::transform(name,name.begin(),[](unsigned char c){return c>='A'&&c<='Z'?c+32:c;});
        require(!folded.contains(name),"Process package file conflicts with directory");
        const auto [found,inserted]=directories.emplace(name,original);
        require(inserted || found->second==original,"Process package directory has case aliases");
    }
    require(arguments.size()<=128,"Too many process package arguments");
    std::size_t argument_bytes=0;
    for (const auto& argument:arguments) {
        require(argument.size()<=32768 && argument.find('\0')==std::string::npos,"Invalid process package argument");
        argument_bytes+=argument.size(); require(argument_bytes<=65536,"Process package arguments exceed byte limit");
        if (argument.starts_with("${package}/")) {
            const auto raw=argument.substr(11);
            const auto path=path_of(raw);
            require(raw==utf8(path),"Noncanonical process package argument path");
            (void)relative_name(path);
        }
        else require(argument.find("${") == std::string::npos,"Unknown process package argument template");
        value->arguments.push_back(argument);
    }
    impl_=std::move(value);
}
const fs::path& ProcessPackage::entrypoint() const {return impl_->entrypoint;}
const fs::path& ProcessPackage::source() const {return impl_->source;}
std::vector<std::string> ProcessPackage::arguments(const fs::path& visible_root) const {
    require(visible_root.is_absolute(),"Process package root must be absolute");
    auto result=impl_->arguments;
    for (auto& argument:result) if (argument.starts_with("${package}/"))
        argument=utf8(visible_root/path_of(argument.substr(11)));
    return result;
}
void ProcessPackage::stage(const fs::path& destination,const std::atomic_bool& cancelled) const {
    check(cancelled);
    require(destination.is_absolute() && destination.lexically_normal()==destination && fs::is_empty(destination),
            "Process package staging requires an empty private directory");
    BuildImportOptions options;
    options.workspace_exclusions=false;
    options.limits.max_files=128;options.limits.max_bytes=byte_limit;options.limits.max_file_bytes=byte_limit;
    options.limits.max_entries=4096;options.limits.max_depth=32;
    for (const auto& file:impl_->files) options.inputs.push_back(file.path);
    import_build_tree(impl_->source,destination,options,cancelled);
    std::size_t total=0;
    for (const auto& file:impl_->files) {
        check(cancelled);
        const auto data=read_build_manifest_snapshot(destination/file.path,byte_limit);
        require(data.size()==file.size && sha256_bytes(data)==file.digest,"Process package changed before isolated staging");
#ifndef _WIN32
        const auto bits=fs::status(destination/file.path).permissions();
        const auto mode=(bits&(fs::perms::owner_exec|fs::perms::group_exec|fs::perms::others_exec))!=fs::perms::none;
        require(mode==file.executable,"Process package executable mode changed before isolated staging");
#endif
        total+=data.size();
    }
    const auto actual=measure_build_tree(destination,options.limits,cancelled);
    require(actual.files==impl_->files.size() && actual.bytes==total,"Process package staged inventory differs");
    check(cancelled);
}
void validate_package_limits(const ProcessRequest& request) {
    const auto& limits=request.package_limits;
    require(limits.processes>=1 && limits.processes<=32 && limits.memory_bytes>=64ULL*1024*1024 &&
            limits.memory_bytes<=4ULL*1024*1024*1024 && limits.cpu_percent>=1 && limits.cpu_percent<=100,
            "Invalid process package resource limits");
    if (!request.package) {
        require(limits.processes==1 && limits.memory_bytes==512ULL*1024*1024 && limits.cpu_percent==80,
                "Package resource limits require a process package");
        return;
    }
    require(request.sandbox!=SandboxMode::none,"Process package requires explicit OS isolation");
    require(request.executable==request.package->source()/request.package->entrypoint() &&
            request.arguments==request.package->arguments(request.package->source()),"Process package launch differs from frozen plan");
}
}
