#pragma once

#ifdef __APPLE__
#include <signal.h>
#include <spawn.h>
#include <system_error>
#include <unistd.h>

namespace codestudio {
// Darwin's spawn path avoids running C++ in a forked, multithreaded host.
// Only the explicitly duplicated standard streams survive exec, including when
// unrelated descriptors in the host were opened without FD_CLOEXEC.
inline pid_t darwin_spawn(const char* executable, char* const argv[], char* const envp[],
                         const char* directory, int input, int output, int errors) {
    const auto check = [](int error, const char* operation) {
        if (error) throw std::system_error(error, std::generic_category(), operation);
    };
    posix_spawn_file_actions_t actions;
    check(posix_spawn_file_actions_init(&actions), "posix_spawn_file_actions_init");
    struct ActionsGuard {
        posix_spawn_file_actions_t* value;
        ~ActionsGuard() { posix_spawn_file_actions_destroy(value); }
    } actions_guard{&actions};
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
    check(posix_spawn_file_actions_addchdir(&actions, directory), "posix_spawn chdir");
#else
    check(posix_spawn_file_actions_addchdir_np(&actions, directory), "posix_spawn chdir");
#endif
    check(posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO), "posix_spawn stdin");
    check(posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO), "posix_spawn stdout");
    check(posix_spawn_file_actions_adddup2(&actions, errors, STDERR_FILENO), "posix_spawn stderr");

    posix_spawnattr_t attributes;
    check(posix_spawnattr_init(&attributes), "posix_spawnattr_init");
    struct AttributesGuard {
        posix_spawnattr_t* value;
        ~AttributesGuard() { posix_spawnattr_destroy(value); }
    } attributes_guard{&attributes};
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigfillset(&defaults);
    check(posix_spawnattr_setsigmask(&attributes, &mask), "posix_spawn signal mask");
    check(posix_spawnattr_setsigdefault(&attributes, &defaults), "posix_spawn signal defaults");
    check(posix_spawnattr_setpgroup(&attributes, 0), "posix_spawn process group");
    check(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
          POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT), "posix_spawn flags");
    pid_t pid = -1;
    check(posix_spawn(&pid, executable, &actions, &attributes, argv, envp), "posix_spawn");
    return pid;
}
}
#endif
