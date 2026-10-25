#include "codestudio/application.hpp"
#include "codestudio/service.hpp"
#include "stream_output.hpp"
#include "credentials_cli.hpp"
#include <csignal>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace codestudio;
std::atomic_bool cancelled{false};
static_assert(std::atomic_bool::is_always_lock_free);
#ifdef _WIN32
BOOL WINAPI on_console_signal(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) { cancelled.store(true); return TRUE; }
    return FALSE;
}
std::string wide_to_utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("Invalid Unicode command argument");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back(); return result;
}
#else
void on_signal(int) { cancelled.store(true); }
#endif
void print(const Json& value) {
    std::cout << value.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
}
int entry(const std::vector<std::string>& args) {
    // The service owns framing and startup errors: never print CLI JSON into
    // its newline-delimited JSON-RPC stdout transport.
    if (args.size() > 1 && args[1] == "serve") return serve_stdio(args, cancelled);
    StreamOutput stream_output;
    try {
        if (const auto credential=credential_cli(args,cancelled)) {
            print(credential->value);
            return credential->exit_code;
        }
        const auto result = execute_command(args, cancelled,
            {.on_text = [&](std::string_view text) { stream_output.append(text); },
             .on_authorization = [&](const Json& notice) { std::cerr << notice.dump() << '\n' << std::flush; }});
        stream_output.finish();
        if (result.value.is_string()) std::cout << result.value.get_ref<const std::string&>();
        else print(result.value);
        return result.exit_code;
    } catch(const std::exception& e) {
        stream_output.finish();
        print(application_redact({{"product","CodeStudio-Agent"},{"error",e.what()}}));
        return 2;
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(on_console_signal,TRUE);
    std::vector<std::string> args;
    try { for(int i=0;i<argc;++i) args.push_back(wide_to_utf8(argv[i])); }
    catch(const std::exception& e) { print({{"error",e.what()}}); return 2; }
    return entry(args);
}
#else
int main(int argc, char** argv) {
    std::signal(SIGINT,on_signal); std::signal(SIGTERM,on_signal);
    return entry(std::vector<std::string>(argv,argv+argc));
}
#endif
