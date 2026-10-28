#include "linux_sandbox.hpp"
#ifdef __linux__
#include "codestudio/build_workspace.hpp"
#include "codestudio/process_package.hpp"
#include "codestudio/sandbox_resources.hpp"
#include "codestudio/store.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace codestudio {
namespace {
namespace fs=std::filesystem;
[[noreturn]] void failure(const char* operation) {
    throw std::system_error(errno,std::generic_category(),operation);
}
int above_stdio(int fd) {
    if (fd<0 || fd>=3) return fd;
    const auto duplicate=fcntl(fd,F_DUPFD_CLOEXEC,3);
    const auto error=errno;close(fd);
    if (duplicate<0) {errno=error;failure("Move sandbox descriptor above stdio");}
    return duplicate;
}
void write_file(const fs::path& path,const std::string& value) {
    const auto fd=open(path.c_str(),O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) failure("Open cgroup control");
    const auto size=write(fd,value.data(),value.size()); const auto error=errno; close(fd);
    if (size!=static_cast<ssize_t>(value.size())) {
        errno=error; failure("Write cgroup control");
    }
}
std::string read_file(const fs::path& path,std::size_t limit=1024*1024) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read Linux isolation configuration: "+path.string());
    std::string result; char bytes[4096];
    while (stream.read(bytes,sizeof bytes) || stream.gcount()) {
        if (result.size()+stream.gcount()>limit) throw std::runtime_error("Linux isolation file exceeds limit");
        result.append(bytes,stream.gcount());
    }
    if (stream.bad()) throw std::runtime_error("Cannot read Linux isolation file");
    return result;
}
fs::path cgroup_parent() {
    if (const auto* path=std::getenv("CODESTUDIO_CGROUP_ROOT"); path && *path) {
        const auto value=fs::canonical(path);
        if (!value.is_absolute() || !value.string().starts_with("/sys/fs/cgroup/"))
            throw std::runtime_error("CODESTUDIO_CGROUP_ROOT must name a delegated cgroup v2 directory");
        return value;
    }
    std::istringstream groups(read_file("/proc/self/cgroup")); std::string line;
    while (std::getline(groups,line)) if (line.starts_with("0::/"))
        return fs::path("/sys/fs/cgroup")/line.substr(4);
    throw std::runtime_error("Linux sandbox requires a delegated cgroup v2 hierarchy");
}
void directory(const fs::path& path) {
    fs::create_directories(path);
    if (chmod(path.c_str(),0700)) failure("Protect sandbox staging directory");
}
struct Elf {
    std::string interpreter;
    std::vector<std::string> needed;
};
// Parse metadata, never run ldd or execute the granted image while staging.
Elf elf(const fs::path& path,bool private_image) {
    // A previous build step can create a FIFO or symlink where the next image
    // is expected. Read untrusted images through the anchored, nofollow,
    // nonblocking regular-file snapshot helper before parsing any bytes.
    const auto bytes=private_image ? read_build_manifest_snapshot(path,256ULL*1024*1024)
                                   : read_file(path,256ULL*1024*1024);
    if (bytes.size()<sizeof(Elf64_Ehdr)) throw std::runtime_error("Linux sandbox tools must be ELF64 executables");
    Elf64_Ehdr header{}; std::memcpy(&header,bytes.data(),sizeof header);
    if (std::memcmp(header.e_ident,ELFMAG,SELFMAG) || header.e_ident[EI_CLASS]!=ELFCLASS64 ||
        header.e_ident[EI_DATA]!=ELFDATA2LSB || header.e_machine!=EM_X86_64 || header.e_phentsize!=sizeof(Elf64_Phdr) ||
        header.e_phnum>1024 || header.e_phoff>bytes.size() || header.e_phnum>(bytes.size()-header.e_phoff)/sizeof(Elf64_Phdr))
        throw std::runtime_error("Linux sandbox requires a valid native x86-64 ELF image");
    std::vector<Elf64_Phdr> headers(header.e_phnum);
    std::memcpy(headers.data(),bytes.data()+header.e_phoff,headers.size()*sizeof(Elf64_Phdr));
    const auto region=[&](std::uint64_t offset,std::uint64_t size) {
        if (offset>bytes.size() || size>bytes.size()-offset) throw std::runtime_error("Invalid ELF segment bounds");
    };
    Elf result; std::vector<Elf64_Xword> strings; std::uint64_t table=0,table_size=0;
    for (const auto& segment:headers) {
        region(segment.p_offset,segment.p_filesz);
        if (segment.p_type==PT_INTERP) {
            if (!segment.p_filesz || segment.p_filesz>4096 || bytes[segment.p_offset+segment.p_filesz-1]!=0)
                throw std::runtime_error("Invalid ELF interpreter");
            result.interpreter=bytes.substr(segment.p_offset,segment.p_filesz-1);
        }
        if (segment.p_type==PT_DYNAMIC) {
            for (std::uint64_t at=0;at+sizeof(Elf64_Dyn)<=segment.p_filesz;at+=sizeof(Elf64_Dyn)) {
                Elf64_Dyn value{}; std::memcpy(&value,bytes.data()+segment.p_offset+at,sizeof value);
                if (value.d_tag==DT_NULL) break;
                if (value.d_tag==DT_NEEDED) strings.push_back(value.d_un.d_val);
                if (value.d_tag==DT_STRTAB) table=value.d_un.d_ptr;
                if (value.d_tag==DT_STRSZ) table_size=value.d_un.d_val;
            }
        }
    }
    if (!strings.empty()) {
        bool found=false;
        for (const auto& segment:headers) if (segment.p_type==PT_LOAD && table>=segment.p_vaddr &&
            table-segment.p_vaddr<segment.p_filesz) {
            const auto offset=segment.p_offset+(table-segment.p_vaddr); region(offset,table_size);
            for (auto index:strings) {
                if (index>=table_size) throw std::runtime_error("Invalid ELF library offset");
                const auto end=bytes.find('\0',offset+index);
                if (end==std::string::npos || end>=offset+table_size) throw std::runtime_error("Invalid ELF library string");
                auto name=bytes.substr(offset+index,end-offset-index);
                if (name.empty() || name.find('/')!=std::string::npos) throw std::runtime_error("ELF dependency must be a library basename");
                result.needed.push_back(std::move(name));
            }
            found=true; break;
        }
        if (!found) throw std::runtime_error("ELF dynamic string table is absent");
    }
    return result;
}
std::vector<sock_filter> filter(bool single) {
    std::vector<sock_filter> rules;
    const auto stmt=[&](unsigned short code,unsigned value){rules.push_back({code,0,0,value});};
    const auto jump=[&](unsigned short code,unsigned value,unsigned char yes,unsigned char no){rules.push_back({code,yes,no,value});};
    stmt(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,arch));
    jump(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0);
    stmt(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS);
    stmt(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr));
    jump(BPF_JMP|BPF_JGE|BPF_K,0x40000000,0,1); stmt(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS);
    std::vector<unsigned> denied{SYS_socket,SYS_connect,SYS_bind,SYS_listen,SYS_accept,SYS_accept4,
        SYS_ptrace,SYS_process_vm_readv,SYS_process_vm_writev,SYS_mount,SYS_umount2,SYS_pivot_root,
        SYS_chroot,SYS_unshare,SYS_setns,SYS_bpf,SYS_perf_event_open,SYS_userfaultfd,
        SYS_keyctl,SYS_add_key,SYS_request_key,SYS_kexec_load,SYS_init_module,SYS_finit_module,
        SYS_delete_module,SYS_reboot,SYS_swapon,SYS_swapoff,SYS_open_by_handle_at,SYS_io_uring_setup,
        SYS_fsopen,SYS_fsmount,SYS_fsconfig,SYS_move_mount,SYS_open_tree,SYS_mount_setattr};
    if (single) {denied.push_back(SYS_clone); denied.push_back(SYS_fork); denied.push_back(SYS_vfork);}
    for (auto call:denied) {jump(BPF_JMP|BPF_JEQ|BPF_K,call,0,1);stmt(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM);}
    // Returning ENOSYS preserves the libc clone fallback while denying the
    // extensible clone3 argument structure (which can create new namespaces).
    jump(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone3,0,1);stmt(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS);
    if (!single) {
        jump(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone,0,4);
        stmt(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,args[0]));
        jump(BPF_JMP|BPF_JSET|BPF_K,0x7e020000U,0,1); // NEW{USER,PID,NET,NS,UTS,IPC,CGROUP}
        stmt(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM);
        stmt(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr));
    }
    stmt(BPF_RET|BPF_K,SECCOMP_RET_ALLOW); return rules;
}
}

struct LinuxSandbox::Impl {
    fs::path root,readonly,writable,runtime,group,image,directory;
    std::map<std::string,std::string> environment;
    std::set<std::string> libraries;
    std::map<std::string,fs::path> package_images;
    std::set<fs::path> prepared_images;
    std::vector<std::string> package_arguments;
    int filter_fd=-1,group_fd=-1;
    bool build=false, tools_prepared=false, package=false;
    const std::atomic_bool* preparing_cancelled=nullptr;
    std::unique_ptr<SandboxResourceLease> resources;
    ~Impl() {
        if (group_fd>=0) close(group_fd);
        if (filter_fd>=0) close(filter_fd);
        bool stopped=group.empty();
        if (!group.empty()) {
            try { write_file(group/"cgroup.kill","1"); } catch (...) {}
            for (int i=0;i<100;++i) {
                std::error_code error;
                if (fs::remove(group,error)) {stopped=true;break;}
                if (!error && !fs::exists(group,error) && !error) {stopped=true;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (stopped && !root.empty()) try {safe_remove_owned_tree(root);} catch (...) {}
        if (resources) try {resources->finish();} catch (...) { /* Durable record remains for explicit recovery. */ }
    }
    void check_preparation() const {
        if (preparing_cancelled && preparing_cancelled->load())
            throw std::runtime_error("Linux sandbox preparation cancelled");
    }
    void runtime_file(const fs::path& source,const fs::path& visible) {
        check_preparation();
        if (!visible.is_absolute() || visible.lexically_normal()!=visible || visible.string().find("..")!=std::string::npos)
            throw std::runtime_error("Invalid sandbox runtime path");
        if (!libraries.insert(visible.string()).second) return;
        const auto actual=fs::canonical(source);
        struct stat state{};
        if (stat(actual.c_str(),&state) || !S_ISREG(state.st_mode) || state.st_uid!=0 || (state.st_mode&0022))
            throw std::runtime_error("Sandbox runtime libraries must be root-owned non-writable regular files");
        const auto target=runtime/visible.relative_path(); directory_create(target.parent_path());
        fs::copy_file(actual,target); if (chmod(target.c_str(),0555)) failure("Protect runtime copy");
        dependencies(actual,false);
    }
    void directory_create(const fs::path& path) { codestudio::directory(path); }
    void dependencies(const fs::path& executable,bool private_image=true) {
        check_preparation();
        // A package is frozen, so cycles can be visited once. Build workspaces
        // may replace an executable between steps and must be parsed anew.
        if (package && !prepared_images.insert(executable).second) return;
        const auto metadata=elf(executable,private_image);
        if (!metadata.interpreter.empty()) {
            const fs::path interpreter(metadata.interpreter);
            bool trusted=false;
            for (const auto* base:{"/lib","/lib64","/usr/lib","/usr/lib64"}) {
                const auto relative=interpreter.lexically_relative(base);
                if (!relative.empty() && *relative.begin()!="..") trusted=true;
            }
            if (!trusted) throw std::runtime_error("ELF interpreter must be in a system library directory");
            runtime_file(interpreter,interpreter);
        }
        for (const auto& library:metadata.needed) {
            check_preparation();
            if (const auto found=package_images.find(library); found!=package_images.end()) {
                dependencies(found->second);
                continue;
            }
            bool found=false;
            for (const auto* base:{"/lib/x86_64-linux-gnu","/usr/lib/x86_64-linux-gnu","/usr/lib64","/usr/lib","/lib64","/lib"}) {
                const auto source=fs::path(base)/library;
                if (fs::is_regular_file(source)) {
                    // Host /lib64 and /usr/lib64 can be aliases for /usr/lib.
                    // The private root has neither those aliases nor ld.so.cache;
                    // retain the SONAME under the canonical system directory.
                    runtime_file(source,fs::canonical(source.parent_path())/library);
                    found=true; break;
                }
            }
            if (!found) throw std::runtime_error("Required ELF library is not in the supported system runtime: "+library);
        }
    }
    void prepare_package(const ProcessPackage& supplied,const std::atomic_bool& cancelled) {
        supplied.stage(readonly,cancelled);
        image=fs::path("/tools")/supplied.entrypoint();
        directory="/work";
        package_arguments=supplied.arguments("/tools");
        std::set<fs::path> search;
        // All inspected bytes come from the hash-checked private copy. Never
        // execute ldd, a package helper, or a host interpreter to discover them.
        for (const auto& entry:fs::recursive_directory_iterator(readonly)) {
            check_preparation();
            if (!entry.is_regular_file()) continue;
            const auto bytes=read_build_manifest_snapshot(entry.path(),64ULL*1024*1024);
            if (bytes.size()<sizeof(Elf64_Ehdr)) continue;
            Elf64_Ehdr header{}; std::memcpy(&header,bytes.data(),sizeof header);
            if (std::memcmp(header.e_ident,ELFMAG,4) ||
                (header.e_type!=ET_EXEC && header.e_type!=ET_DYN)) continue;
            const auto name=entry.path().filename().string();
            if (!package_images.emplace(name,entry.path()).second)
                throw std::runtime_error("Package ELF image basenames must be unique for unambiguous dependency lookup");
            const auto relative=entry.path().parent_path().lexically_relative(readonly);
            search.insert((fs::path("/tools")/relative).lexically_normal());
        }
        const auto entry=readonly/supplied.entrypoint();
        if (!package_images.contains(entry.filename().string()) || package_images.at(entry.filename().string())!=entry)
            throw std::runtime_error("Linux package entrypoint must be a native ELF executable; bundle an interpreter for scripts");
        dependencies(entry);
        for (const auto& [name,path]:package_images) dependencies(path);
        // Search only directories of this frozen package. No host PATH or
        // LD_* injection variables enter the private runtime.
        std::string loader_path;
        for (const auto& path:search) {
            if (!loader_path.empty()) loader_path+=':';
            loader_path+=path.string();
        }
        if (!loader_path.empty()) environment["LD_LIBRARY_PATH"]=std::move(loader_path);
        check_preparation();
    }
};

void LinuxSandbox::validate_available() {
    if (access("/usr/bin/bwrap",X_OK)) throw std::runtime_error("Linux isolation requires /usr/bin/bwrap (bubblewrap)");
    const auto parent=cgroup_parent();
    const auto controllers=read_file(parent/"cgroup.subtree_control");
    for (const auto* controller:{"cpu","memory","pids"})
        if (controllers.find(controller)==std::string::npos)
            throw std::runtime_error("Linux isolation requires delegated cpu, memory and pids controllers; set CODESTUDIO_CGROUP_ROOT to a prepared delegation");
    if (access(parent.c_str(),W_OK)) throw std::runtime_error("Linux isolation cgroup delegation is not writable");
}
LinuxSandbox::LinuxSandbox(const ProcessRequest& request,const LinuxBuildLimits* build,
                           const std::atomic_bool* cancelled):impl_(std::make_unique<Impl>()) {
    validate_package_limits(request);
    if (build && request.package) throw std::invalid_argument("Process package cannot be combined with a workspace build sandbox");
    if (cancelled && cancelled->load()) throw std::runtime_error("Linux sandbox preparation cancelled");
    validate_available(); auto& value=*impl_; value.build=build!=nullptr;
    value.package=static_cast<bool>(request.package); value.preparing_cancelled=cancelled;
    if (build && build->processes<3) throw std::runtime_error("Linux build process limit must include Bubblewrap supervisors and be at least 3");
    const auto resource_id=new_id();
    const auto identifier="codestudio-isolation-"+resource_id;
    value.resources=std::make_unique<SandboxResourceLease>(resource_id,build?"build":request.package?"package":"process",
        request.working_directory,request.task_id);
    const auto group=cgroup_parent()/identifier;
    value.resources->expect("cgroup",group);
    if (!fs::create_directory(group)) throw std::runtime_error("Cannot create owned isolation cgroup");
    value.group=group;
    value.resources->track("cgroup",group);
    write_file(value.group/"pids.max",std::to_string(build ? build->processes : value.package ? request.package_limits.processes+2 : 3));
    write_file(value.group/"memory.max",std::to_string(build ? build->memory_bytes : value.package ? request.package_limits.memory_bytes : 512ULL*1024*1024));
    write_file(value.group/"memory.swap.max","0");
    write_file(value.group/"cpu.max",std::to_string((build ? build->cpu_percent : value.package ? request.package_limits.cpu_percent : 80)*1000)+" 100000");
    write_file(value.group/"memory.oom.group","1");
    value.group_fd=above_stdio(open((value.group/"cgroup.procs").c_str(),O_WRONLY|O_CLOEXEC|O_NOFOLLOW));
    if (value.group_fd<0) failure("Open owned cgroup membership");
    const auto root=fs::temp_directory_path()/identifier;
    value.resources->expect("staging",root);
    if (!fs::create_directory(root)) throw std::runtime_error("Cannot create private Linux isolation directory");
    value.root=root;
    directory(value.root); value.readonly=value.root/"tools"; value.writable=value.root/"work"; value.runtime=value.root/"runtime";
    value.resources->track("staging",root);
    for (const auto& path:{value.readonly,value.writable,value.runtime}) directory(path);
    for (const auto* name:{"proc","dev","work","tools","tmp"}) directory(value.runtime/name);
    const auto rules=filter(!build && (!value.package || request.package_limits.processes==1));
    value.filter_fd=above_stdio(memfd_create("codestudio-seccomp",MFD_CLOEXEC|MFD_ALLOW_SEALING));
    if (value.filter_fd<0) failure("Create seccomp filter");
    const auto size=rules.size()*sizeof(sock_filter);
    if (write(value.filter_fd,rules.data(),size)!=static_cast<ssize_t>(size) || lseek(value.filter_fd,0,SEEK_SET)<0 ||
        fcntl(value.filter_fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)<0) failure("Seal seccomp filter");
    if (!build) {
        value.environment={{"HOME","/work"},{"TMPDIR","/work"},{"TMP","/work"},{"TEMP","/work"},{"PATH","/tools"},{"LANG","C.UTF-8"}};
        const std::atomic_bool not_cancelled{false};
        const auto& preparation_cancelled=cancelled ? *cancelled : not_cancelled;
        if (request.package) value.prepare_package(*request.package,preparation_cancelled);
        else {
            BuildImportOptions options; options.inputs={request.executable.filename()}; options.workspace_exclusions=false;
            options.limits.max_files=1; options.limits.max_bytes=256ULL*1024*1024;
            import_build_tree(request.executable.parent_path(),value.readonly,options,preparation_cancelled);
            value.image=fs::path("/tools")/request.executable.filename(); value.directory="/work";
            value.dependencies(value.readonly/request.executable.filename());
        }
    }
    value.check_preparation();value.resources->mark_ready();value.preparing_cancelled=nullptr;
}
LinuxSandbox::~LinuxSandbox()=default;
const fs::path& LinuxSandbox::readonly_root() const {return impl_->readonly;}
const fs::path& LinuxSandbox::writable_root() const {return impl_->writable;}
fs::path LinuxSandbox::build_path(const fs::path& physical) const {
    const auto path=physical.lexically_normal();
    for (const auto& [base,target]:{std::pair{impl_->readonly,fs::path("/tools")},std::pair{impl_->writable,fs::path("/work")}}) {
        auto relative=path.lexically_relative(base);
        if (!relative.empty() && *relative.begin()!="..") return target/relative;
    }
    throw std::runtime_error("Build sandbox path is outside its private tree");
}
void LinuxSandbox::configure_build(const fs::path& executable,const fs::path& cwd,const std::map<std::string,std::string>& environment) {
    if (!impl_->build) throw std::logic_error("Not a build sandbox");
    impl_->image=build_path(executable); impl_->directory=build_path(cwd); impl_->environment=environment;
    impl_->dependencies(executable);
    if (!impl_->tools_prepared) {
        // Compilers launch helpers such as cc1plus/as/ld. Discover dependencies
        // from copied tool images without running them or mounting host /usr.
        for (const auto& entry:fs::recursive_directory_iterator(impl_->readonly)) {
            if (!entry.is_regular_file()) continue;
            std::ifstream input(entry.path(),std::ios::binary);
            Elf64_Ehdr header{}; input.read(reinterpret_cast<char*>(&header),sizeof header);
            // Relocatable objects (crt*.o and library members) are linker data,
            // not runtime images; they deliberately have no program headers.
            if (input.gcount()==sizeof header && std::memcmp(header.e_ident,ELFMAG,4)==0 &&
                (header.e_type==ET_EXEC || header.e_type==ET_DYN)) impl_->dependencies(entry.path());
        }
        impl_->tools_prepared=true;
    }
}
std::vector<std::string> LinuxSandbox::arguments(const ProcessRequest& request) const {
    const auto& value=*impl_;
    std::vector<std::string> result{"/usr/bin/bwrap","--unshare-all","--die-with-parent","--new-session",
        "--cap-drop","ALL","--clearenv","--ro-bind",value.runtime.string(),"/","--proc","/proc","--dev","/dev",
        "--bind",value.writable.string(),"/work","--ro-bind",value.readonly.string(),"/tools",
        "--chdir",value.directory.string(),"--seccomp",std::to_string(value.filter_fd)};
    for (const auto& [key,text]:value.environment) result.insert(result.end(),{"--setenv",key,text});
    result.push_back("--"); result.push_back(value.image.string());
    const auto& child_arguments=value.package ? value.package_arguments : request.arguments;
    result.insert(result.end(),child_arguments.begin(),child_arguments.end()); return result;
}
int LinuxSandbox::child_setup() const noexcept {
    if (write(impl_->group_fd,"0",1)!=1) return errno;
    if (lseek(impl_->filter_fd,0,SEEK_SET)<0) return errno;
    if (fcntl(impl_->filter_fd,F_SETFD,0)<0) return errno;
    return 0;
}
void LinuxSandbox::terminate() noexcept {try {write_file(impl_->group/"cgroup.kill","1");} catch (...) {}}
bool LinuxSandbox::populated() const {return read_file(impl_->group/"cgroup.events").find("populated 1")!=std::string::npos;}
void LinuxSandbox::quiesce() {
    if (populated()) terminate();
    for (int i=0;i<500;++i) {if (!populated()) return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    throw std::runtime_error("Linux isolation process tree did not quiesce; artifact export refused");
}
}
#endif
