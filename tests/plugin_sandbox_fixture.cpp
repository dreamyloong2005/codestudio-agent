#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#ifdef __linux__
#include <sys/prctl.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
fs::path path(const std::string& text) { return fs::path(std::u8string(text.begin(),text.end())); }
std::string utf8(const fs::path& value) {
    const auto text=value.u8string(); return {text.begin(),text.end()};
}
void send(const Json& value) { std::cout<<value.dump()<<'\n'<<std::flush; }
void reply(const Json& id,const Json& value) { send({{"jsonrpc","2.0"},{"id",id},{"result",value}}); }
std::string read(const fs::path& source) {
    std::ifstream input(source,std::ios::binary);
    if (!input) throw std::runtime_error("Fixture could not read its packaged input");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
fs::path executable() {
#ifdef _WIN32
    std::wstring text(32768,L'\0');
    const auto size=GetModuleFileNameW(nullptr,text.data(),static_cast<DWORD>(text.size()));
    if (!size || size==text.size()) throw std::runtime_error("Fixture image lookup failed");
    text.resize(size);return text;
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string text(size, '\0');
    if (_NSGetExecutablePath(text.data(), &size)) throw std::runtime_error("Fixture image lookup failed");
    return fs::canonical(text.c_str());
#else
    std::string text(32768,'\0');
    const auto size=readlink("/proc/self/exe",text.data(),text.size());
    if (size<=0 || static_cast<std::size_t>(size)==text.size()) throw std::runtime_error("Fixture image lookup failed");
    text.resize(size);return text;
#endif
}
unsigned long pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}
int open_error(const fs::path& target,bool writing,bool create=false) {
#ifdef _WIN32
    const auto file=CreateFileW(target.c_str(),writing?GENERIC_WRITE:GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,create?CREATE_NEW:OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
    CloseHandle(file);return 0;
#else
    const auto file=open(target.c_str(),(writing?O_WRONLY:O_RDONLY)|(create?O_CREAT|O_EXCL:0),0600);
    if (file<0) return errno;
    close(file);return 0;
#endif
}
Json identity() {
#ifdef _WIN32
    char* secret=nullptr;std::size_t secret_size=0;
    const bool secret_present=_dupenv_s(&secret,&secret_size,"CODESTUDIO_PLUGIN_TEST_SECRET")==0 && secret!=nullptr;
    std::free(secret);
#else
    const bool secret_present=std::getenv("CODESTUDIO_PLUGIN_TEST_SECRET")!=nullptr;
#endif
    Json result{{"pid",pid()},{"cwd",utf8(fs::current_path())},{"image",utf8(executable())},
        {"secret_present",secret_present}};
#ifdef _WIN32
    HANDLE token=nullptr;DWORD app=0,size=0;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) throw std::runtime_error("Fixture token lookup failed");
    const auto ok=GetTokenInformation(token,TokenIsAppContainer,&app,sizeof(app),&size);CloseHandle(token);
    if (!ok) throw std::runtime_error("Fixture identity query failed");
    result["appcontainer"]=app!=0;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr))
        throw std::runtime_error("Fixture job query failed");
    result["process_limit"]=limits.BasicLimitInformation.ActiveProcessLimit;
    result["memory_limit"]=limits.JobMemoryLimit;
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    if (!QueryInformationJobObject(nullptr,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu),nullptr))
        throw std::runtime_error("Fixture CPU job query failed");
    result["cpu_percent"]=cpu.CpuRate/100;
#elif defined(__linux__)
    result["seccomp"]=prctl(PR_GET_SECCOMP);
    result["no_new_privileges"]=prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0);
#endif
    return result;
}
Json network(unsigned short port) {
    Json result;
#ifdef _WIN32
    WSADATA data{};const auto startup=WSAStartup(MAKEWORD(2,2),&data);
    result["startup"]=startup;
    if (startup) return result;
    const auto connection=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (connection==INVALID_SOCKET) {result["socket_error"]=WSAGetLastError();WSACleanup();return result;}
    result["socket_error"]=0;
    u_long enabled=1;ioctlsocket(connection,FIONBIO,&enabled);
    sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(port);
    auto error=connect(connection,reinterpret_cast<sockaddr*>(&target),sizeof(target))==0?0:WSAGetLastError();
    if (error==WSAEWOULDBLOCK) {
        fd_set writes,exceptions;FD_ZERO(&writes);FD_ZERO(&exceptions);FD_SET(connection,&writes);FD_SET(connection,&exceptions);
        timeval timeout{0,300000};const auto ready=select(0,nullptr,&writes,&exceptions,&timeout);
        if (ready>0) {int size=sizeof(error);if(getsockopt(connection,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size))error=WSAGetLastError();}
        else error=ready==0?WSAETIMEDOUT:WSAGetLastError();
    }
    result["connect_error"]=error;closesocket(connection);WSACleanup();
#else
    const auto connection=socket(AF_INET,SOCK_STREAM,0);
    result["socket_error"]=connection<0?errno:0;
    if(connection>=0) {sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(port);
        result["connect_error"]=connect(connection,reinterpret_cast<sockaddr*>(&target),sizeof(target))<0?errno:0;close(connection);}
#endif
    return result;
}
Json spawn(bool blocking) {
    const auto marker=fs::current_path()/"child.started";
    Json result{{"child_created",false},{"marker",utf8(marker)}};
#ifdef _WIN32
    const auto image=executable();
    auto command=L"\""+image.native()+L"\" "+(blocking?L"--child-block":L"--child")+L" \""+marker.native()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    if(!CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)){
        result["spawn_error"]=GetLastError();return result;}
    CloseHandle(child.hThread);result["child_created"]=true;result["spawn_error"]=0;
    if(!blocking){const auto wait=WaitForSingleObject(child.hProcess,5000);DWORD code=999;GetExitCodeProcess(child.hProcess,&code);
        result["child_exit"]=code;if(wait!=WAIT_OBJECT_0){TerminateProcess(child.hProcess,9);WaitForSingleObject(child.hProcess,5000);}}
    CloseHandle(child.hProcess);
#else
    const auto image=executable();const auto child=fork();
    if(child<0){result["spawn_error"]=errno;return result;}
    if(child==0){execl(image.c_str(),image.c_str(),blocking?"--child-block":"--child",marker.c_str(),nullptr);_exit(77);}
    result["child_created"]=true;result["spawn_error"]=0;
    if(!blocking){int state=0;waitpid(child,&state,0);result["child_exit"]=WIFEXITED(state)?WEXITSTATUS(state):999;}
#endif
    if(!blocking)result["child_marker"]=read(marker);
    return result;
}
int library_value(const fs::path& library) {
    using Value=int(*)();
#ifdef _WIN32
    const auto module=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module)throw std::runtime_error("Fixture private library load failed");
    const auto entry=reinterpret_cast<Value>(GetProcAddress(module,"codestudio_plugin_fixture_value"));
    if(!entry){FreeLibrary(module);throw std::runtime_error("Fixture library export is missing");}
    const auto value=entry();FreeLibrary(module);return value;
#else
    const auto module=dlopen(library.c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!module)throw std::runtime_error("Fixture private library load failed");
    const auto entry=reinterpret_cast<Value>(dlsym(module,"codestudio_plugin_fixture_value"));
    if(!entry){dlclose(module);throw std::runtime_error("Fixture library export is missing");}
    const auto value=entry();dlclose(module);return value;
#endif
}
Json definition(const std::string& name) {
    Json properties=Json::object(),required=Json::array();
    if(name=="probe"){
        properties={{"outside_file",{{"type","string"}}},{"outside_new",{{"type","string"}}},
                    {"port",{{"type","integer"},{"minimum",1},{"maximum",65535}}}};
        required=Json::array({"outside_file","outside_new","port"});
    }
    return {{"name",name},{"description","Owned package sandbox fixture"},
        {"inputSchema",{{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}}};
}
int run(const std::vector<std::string>& arguments) {
    if(arguments.size()==3 && (arguments[1]=="--child" || arguments[1]=="--child-block")){
        std::ofstream marker(path(arguments[2]));marker<<"child "<<pid();marker.close();
        if(arguments[1]=="--child-block")std::this_thread::sleep_for(std::chrono::seconds(30));
        return marker?0:5;
    }
    if(arguments.size()!=3)return 2;
    const auto config_path=path(arguments[1]),library_path=path(arguments[2]);
    const auto config=Json::parse(read(config_path));
    const auto value=library_value(library_path);
    const auto scratch=fs::current_path()/"owned-scratch.txt";
    const bool scratch_existed=fs::exists(scratch);
    std::ofstream scratch_output(scratch);scratch_output<<"private scratch";scratch_output.close();
    if(!scratch_output)throw std::runtime_error("Fixture scratch is not writable");
    for(std::string line;std::getline(std::cin,line);){
        const auto request=Json::parse(line);const auto method=request.value("method","");
        if(!request.contains("id"))continue;
        const auto id=request.at("id");
        if(method=="initialize"){
            if(config.value("hang_initialize",false))std::this_thread::sleep_for(std::chrono::seconds(30));
            reply(id,{{"protocolVersion","2025-11-25"},{"capabilities",{{"tools",Json::object()}}},
                {"serverInfo",{{"name","isolated-package-fixture"},{"version","1.0.0"}}}});
        }else if(method=="ping")reply(id,Json::object());
        else if(method=="tools/list")reply(id,{{"tools",Json::array({definition("probe"),definition("spawn"),definition("hang"),definition("hang_child")})}});
        else if(method=="tools/call"){
            const auto name=request.at("params").at("name").get<std::string>();
            Json report;
            if(name=="hang" || name=="hang_child"){
                if(name=="hang_child"){
                    report=spawn(true);
                    if(!report.at("child_created").get<bool>())throw std::runtime_error("Fixture child could not start");
                }
                std::ofstream(fs::current_path()/"call.started")<<"dispatched";
                std::this_thread::sleep_for(std::chrono::seconds(30));
            }else if(name=="spawn")report=spawn(false);
            else if(name=="probe"){
                report=identity();const auto& input=request.at("params").at("arguments");
                report.update({{"config_value",config.at("value")},{"library_value",value},
                    {"config_path",utf8(config_path)},{"library_path",utf8(library_path)},
                    {"package_write",open_error(config_path,true)},
                    {"package_create",open_error(config_path.parent_path()/"forbidden.txt",true,true)},
                    {"image_write",open_error(executable(),true)},
                    {"outside_read",open_error(path(input.at("outside_file")),false)},
                    {"outside_write",open_error(path(input.at("outside_file")),true)},
                    {"outside_create",open_error(path(input.at("outside_new")),true,true)},
                    {"scratch_value",read(scratch)},{"scratch_existed",scratch_existed},
                    {"network",network(input.at("port").get<unsigned short>())}});
            }else throw std::runtime_error("Fixture tool not found");
            reply(id,{{"content",Json::array({{{"type","text"},{"text",report.dump()}}})},
                {"structuredContent",report},{"isError",false}});
        }else throw std::runtime_error("Fixture method not found");
    }
    return 0;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){
    try{std::vector<std::string> args;for(int i=0;i<argc;++i)args.push_back(utf8(fs::path(argv[i])));return run(args);}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 3;}
}
#else
int main(int argc,char** argv){
    try{return run(std::vector<std::string>(argv,argv+argc));}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 3;}
}
#endif
