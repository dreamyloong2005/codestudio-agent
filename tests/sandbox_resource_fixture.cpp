#include "codestudio/line_process.hpp"
#include "codestudio/process_observation.hpp"
#include "codestudio/sandbox_resources.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using codestudio::Json;
namespace fs=std::filesystem;
std::string utf8(const fs::path& path) {
    const auto value=path.generic_u8string();return {value.begin(),value.end()};
}
fs::path path(const std::string& value) {return fs::path(std::u8string(value.begin(),value.end()));}
void emit(const Json& value) {std::cout<<value.dump()<<'\n'<<std::flush;}
std::int64_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}
int run(const std::vector<std::string>& arguments) {
    if(arguments.size()==3 && arguments[1]=="cancel") {
        const std::atomic_bool cancelled{true};
        emit(codestudio::reclaim_sandbox_resource(arguments[2],false,cancelled));
        return 0;
    }
    if(arguments.size()==2 && arguments[1]=="identity") {
        emit({{"pid",process_id()},{"identity",codestudio::current_process_identity()}});return 0;
    }
    if(arguments.size()==4 && (arguments[1]=="plain-run" || arguments[1]=="plain-line")) {
        const std::atomic_bool cancelled{false};
        codestudio::ProcessRequest request;
        request.executable=fs::canonical(path(arguments[2]));
        request.arguments={"sleep","10000"};
        request.working_directory=fs::canonical(path(arguments[3]));
        request.timeout=std::chrono::seconds(15);
        if(arguments[1]=="plain-run")return codestudio::run_process(request,cancelled).exit_code;
        codestudio::LineProcess child(request,&cancelled);
        if(child.receive(std::chrono::steady_clock::now()+std::chrono::seconds(5),cancelled)!="started")
            throw std::runtime_error("Plain process fixture did not reach its wait");
        std::string stop;std::getline(std::cin,stop);
        return 0;
    }
    if(arguments.size()!=4 || arguments[1]!="hold")return 2;
    const std::atomic_bool cancelled{false};
    codestudio::ProcessRequest request;
    request.executable=fs::canonical(path(arguments[2]));
    request.arguments={"sleep"};request.working_directory=fs::canonical(path(arguments[3]));
    request.timeout=std::chrono::seconds(60);request.task_id="owned-resource-fixture-task";
#ifdef _WIN32
    request.sandbox=codestudio::SandboxMode::appcontainer;
#else
    request.sandbox=codestudio::SandboxMode::linux_namespace;
#endif
    {
        codestudio::LineProcess child(request,&cancelled);
        const auto identity=Json::parse(child.receive(std::chrono::steady_clock::now()+std::chrono::seconds(8),cancelled));
        if(!identity.value("sleeping",false))throw std::runtime_error("Sandbox fixture did not reach its wait");
        Json owned=nullptr;
        const auto listing=codestudio::list_sandbox_resources({},100);
        for(const auto& item:listing.at("items")) {
            if(item.contains("owner") && item["owner"].value("pid",std::int64_t{})==process_id()) {
                if(!owned.is_null())throw std::runtime_error("Fixture unexpectedly owns multiple resources");
                owned=item;
            }
        }
        if(owned.is_null())throw std::runtime_error("Sandbox has no ownership journal");
        emit({{"resource",owned},{"sandbox",identity},{"workspace",utf8(request.working_directory)}});
        std::string stop;std::getline(std::cin,stop);
    }
    emit({{"finished",true}});return 0;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    try {
        std::vector<std::string> arguments;
        for(int index=0;index<argc;++index)arguments.push_back(utf8(fs::path(argv[index])));
        return run(arguments);
    } catch(const std::exception& error) {emit({{"fixture_error",error.what()}});return 3;}
}
#else
int main(int argc,char** argv) {
    try {return run(std::vector<std::string>(argv,argv+argc));}
    catch(const std::exception& error) {emit({{"fixture_error",error.what()}});return 3;}
}
#endif
