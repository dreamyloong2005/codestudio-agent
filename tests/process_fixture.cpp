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
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
std::filesystem::path from_utf8(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

int run_fixture(const std::vector<std::string>& args, const std::filesystem::path& self) {
    if (args.empty()) return 2;
    if (args[0] == "echo") {
        for (std::size_t i = 1; i < args.size(); ++i) std::cout << args[i].size() << ':' << args[i] << '\n';
    } else if (args[0] == "spam") {
        const std::string block(8192, 'x');
        for (int i = 0; i < 512; ++i) std::cout << block;
        std::cerr << "stderr-marker";
    } else if (args[0] == "bytes") {
        std::cout.put(static_cast<char>(0xff));
        std::cout.put(static_cast<char>(0xfe));
    } else if (args[0] == "streams") {
        std::cout << "stdout-marker" << std::flush;
        std::cerr << "stderr-marker" << std::flush;
    } else if (args[0] == "sleep") {
        std::cout << "started\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(args.at(1))));
    } else if (args[0] == "exit") {
        return std::stoi(args.at(1));
    } else if (args[0] == "environment") {
#ifdef _WIN32
        std::cout << (GetEnvironmentVariableW(L"CODESTUDIO_API_KEY", nullptr, 0) ? "present" : "absent") << '\n';
        std::cout << (GetEnvironmentVariableW(L"OPENAI_API_KEY", nullptr, 0) ? "present" : "absent") << '\n';
#else
        std::cout << (std::getenv("CODESTUDIO_API_KEY") ? "present" : "absent") << '\n';
        std::cout << (std::getenv("OPENAI_API_KEY") ? "present" : "absent") << '\n';
#endif
    } else if (args[0] == "touch-later") {
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        std::ofstream marker(from_utf8(args.at(1)));
        marker << "descendant survived";
    } else if (args[0] == "child") {
#ifdef _WIN32
        const auto marker = from_utf8(args.at(1)).native();
        std::wstring command = L"\"" + self.native() + L"\" touch-later \"" + marker + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &info)) return 3;
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
#else
        const auto pid = fork();
        if (pid < 0) return 3;
        if (pid == 0) {
            execl(self.c_str(), self.c_str(), "touch-later", args.at(1).c_str(), nullptr);
            _exit(4);
        }
#endif
        std::cout << "descendant-started\n" << std::flush;
        // Parent exits immediately: the runner must still supervise the descendant.
    } else return 2;
    return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0) return 5;
        std::string value(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), length, nullptr, nullptr);
        value.pop_back();
        args.push_back(std::move(value));
    }
    return run_fixture(args, std::filesystem::absolute(argv[0]));
}
#else
int main(int argc, char** argv) {
    return run_fixture(std::vector<std::string>(argv + 1, argv + argc), std::filesystem::absolute(argv[0]));
}
#endif
