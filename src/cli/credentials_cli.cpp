#include "credentials_cli.hpp"
#include "codestudio/credentials.hpp"
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace codestudio {
namespace {
std::string secret_input(const std::atomic_bool& cancelled) {
#ifdef _WIN32
    const auto input=GetStdHandle(STD_INPUT_HANDLE);
    const auto type=GetFileType(input);
    if (type!=FILE_TYPE_PIPE && type!=FILE_TYPE_DISK)
        throw std::runtime_error("Credential input requires a redirected stdin pipe or file");
#else
    if (isatty(STDIN_FILENO))
        throw std::runtime_error("Credential input requires redirected stdin; interactive echo is not supported");
#endif
    std::string value;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for (;;) {
        if (cancelled.load()) throw std::runtime_error("Credential input cancelled");
        if (std::chrono::steady_clock::now()>=deadline)
            throw std::runtime_error("Credential input deadline exceeded");
        std::array<char,256> buffer{};
#ifdef _WIN32
        if (type==FILE_TYPE_PIPE) {
            DWORD available=0;
            if (!PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr)) {
                if (GetLastError()==ERROR_BROKEN_PIPE) break;
                throw std::runtime_error("Cannot read credential input");
            }
            if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        }
        DWORD received=0;
        if (!ReadFile(input,buffer.data(),static_cast<DWORD>(buffer.size()),&received,nullptr)) {
            if (GetLastError()==ERROR_BROKEN_PIPE) break;
            throw std::runtime_error("Cannot read credential input");
        }
#else
        pollfd descriptor{STDIN_FILENO,POLLIN,0};
        const auto ready=poll(&descriptor,1,100);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0 || (descriptor.revents&(POLLERR|POLLNVAL)))
            throw std::runtime_error("Cannot read credential input");
        if (!ready) continue;
        const auto received=read(STDIN_FILENO,buffer.data(),buffer.size());
        if (received<0 && errno==EINTR) continue;
        if (received<0) throw std::runtime_error("Cannot read credential input");
#endif
        if (!received) break;
        value.append(buffer.data(),static_cast<std::size_t>(received));
        if (value.size()>2050) throw std::runtime_error("Credential input exceeds size limit");
    }
    if (value.ends_with('\n')) { value.pop_back(); if (value.ends_with('\r')) value.pop_back(); }
    validate_credential_secret(value);
    return value;
}
}

std::optional<CommandResult> credential_cli(const std::vector<std::string>& args,
                                           const std::atomic_bool& cancelled) {
    if (args.size()<2 || !args[1].starts_with("credential-")) return std::nullopt;
    const auto& command=args[1];
    if (command!="credential-set" && command!="credential-delete" && command!="credential-status")
        throw std::runtime_error("Unknown credential command");
    std::string selected;
    bool input=false;
    for (std::size_t i=2;i<args.size();++i) {
        if (args[i]=="--stdin" && command=="credential-set" && !input) { input=true; continue; }
        const auto option=command=="credential-status" ? "--credential" : "--name";
        if (args[i]!=option || !selected.empty() || i+1>=args.size())
            throw std::runtime_error("Invalid credential command options");
        selected=args[++i];
        if (selected.empty()) throw std::runtime_error("Credential reference or name is required");
    }
    if (selected.empty() || (command=="credential-set" && !input))
        throw std::runtime_error("Credential command requires a reference/name; setting requires --stdin");
    const auto reference=parse_credential_reference(command=="credential-status" ? selected : "system:"+selected);
    const auto backend=reference.kind=="env" ? "environment" : system_credential_backend();
    std::string state;
    if (cancelled.load()) throw std::runtime_error("Credential command cancelled");
    if (command=="credential-status") state=read_credential(reference).state;
    else if (command=="credential-set") {
        const auto secret=secret_input(cancelled);
        if (cancelled.load()) throw std::runtime_error("Credential command cancelled");
        state=write_system_credential(reference.name,secret);
    } else state=delete_system_credential(reference.name);
    return CommandResult{Json{{"reference",reference.text()},{"backend",backend},{"state",state}},
        state=="available" || state=="deleted" || state=="missing" ? 0 : 2};
}
}
