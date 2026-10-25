#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
std::filesystem::path utf8_path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
#ifdef _WIN32
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
#endif

int run(const std::vector<std::string>& arguments) {
    using namespace std::chrono_literals;
    if (arguments.size() < 2) return 2;
    const auto& mode = arguments[1];
    if (mode == "hang") { std::this_thread::sleep_for(30s); return 0; }
    if (mode == "exit") return 17;
    if (mode == "partial") { std::cout << "unfinished" << std::flush; return 0; }
    if (mode == "invalid") { std::cout << "\xc0\xaf\n" << std::flush; return 0; }
    if (mode == "crlf") { std::cout << "first\r\nsecond\n" << std::flush; return 0; }
    if (mode == "args") {
        for (std::size_t i = 2; i < arguments.size(); ++i)
            std::cout << arguments[i].size() << ':' << arguments[i] << '\n';
        std::cout << std::flush;
        return 0;
    }
    if (mode == "env") {
        for (const auto* name : {"CODESTUDIO_API_KEY", "OPENAI_API_KEY", "ANTHROPIC_API_KEY", "AWS_SECRET_ACCESS_KEY",
                                "GITHUB_TOKEN", "NPM_TOKEN", "UNRELATED_CUSTOM_SECRET"}) {
#ifdef _WIN32
            const std::string key(name);
            const std::wstring wide_key(key.begin(), key.end());
            const bool present = GetEnvironmentVariableW(wide_key.c_str(), nullptr, 0) > 0;
#else
            const bool present = std::getenv(name) != nullptr;
#endif
            std::cout << name << ':' << (present ? "present" : "absent") << '\n';
        }
        std::cout << std::flush;
        return 0;
    }
    if (mode == "flood" || mode == "queued") {
        const std::string block = mode == "flood" ? std::string(8192, 'x') : std::string(8192, '\n');
        for (int i = 0; i < 256; ++i) std::cout << block;
        std::cout << std::flush;
        std::this_thread::sleep_for(30s);
        return 0;
    }
    if (mode == "stderr") {
        const std::string block(8192, 's');
        std::cerr << "private stderr marker";
        for (int i = 0; i < 256; ++i) std::cerr << block;
        std::cerr << std::flush;
    }
    if (mode == "marker" && arguments.size() == 3) {
        std::this_thread::sleep_for(800ms);
        std::ofstream marker(utf8_path(arguments[2]));
        marker << "descendant survived";
        return 0;
    }
    if (mode == "child" && arguments.size() == 3) {
#ifdef _WIN32
        const auto executable = utf8_path(arguments[0]);
        auto command = quote(executable.native()) + L" marker " + quote(utf8_path(arguments[2]).native());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &info)) return 3;
        CloseHandle(info.hThread); CloseHandle(info.hProcess);
#else
        const auto pid = fork();
        if (pid < 0) return 3;
        if (pid == 0) { execl(arguments[0].c_str(), arguments[0].c_str(), "marker", arguments[2].c_str(), nullptr); _exit(127); }
#endif
        std::cout << "descendant-started\n" << std::flush;
        return 0;
    }
    if (mode == "echo" || mode == "stderr") {
        std::string line;
        while (std::getline(std::cin, line)) std::cout << line << '\n' << std::flush;
        return 0;
    }
    return 2;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i) {
        const auto length = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string value(length, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), length, nullptr, nullptr);
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    return run(arguments);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
