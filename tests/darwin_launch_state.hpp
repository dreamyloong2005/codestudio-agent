#pragma once

#ifdef __APPLE__
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <unistd.h>

// Exercise the desktop-host conditions that differ from a simple terminal:
// closed stdin, an inheritable descriptor, and customized signal policy.
struct DarwinLaunchState {
    int saved_input = -1;
    int sentinel = -1;
    struct sigaction old_pipe{};
    sigset_t old_mask{};
    bool changed_input = false, changed_pipe = false, changed_mask = false;

    DarwinLaunchState() {
        try {
            saved_input = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
            if (saved_input < 0) throw std::runtime_error("save stdin for launch fixture");
            sentinel = fcntl(saved_input, F_DUPFD, 200);
            if (sentinel < 0) throw std::runtime_error("create inheritable descriptor fixture");
            struct sigaction ignored{};
            ignored.sa_handler = SIG_IGN;
            sigemptyset(&ignored.sa_mask);
            if (sigaction(SIGPIPE, &ignored, &old_pipe) != 0) throw std::runtime_error("set SIGPIPE fixture");
            changed_pipe = true;
            sigset_t blocked;
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGUSR1);
            if (pthread_sigmask(SIG_BLOCK, &blocked, &old_mask) != 0) throw std::runtime_error("set signal mask fixture");
            changed_mask = true;
            if (close(STDIN_FILENO) != 0) throw std::runtime_error("close stdin fixture");
            changed_input = true;
        } catch (...) { restore(); throw; }
    }
    ~DarwinLaunchState() { restore(); }
    DarwinLaunchState(const DarwinLaunchState&) = delete;
    DarwinLaunchState& operator=(const DarwinLaunchState&) = delete;
private:
    void restore() {
        if (changed_input) dup2(saved_input, STDIN_FILENO);
        if (changed_mask) pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
        if (changed_pipe) sigaction(SIGPIPE, &old_pipe, nullptr);
        if (sentinel >= 0) close(sentinel);
        if (saved_input >= 0) close(saved_input);
    }
};
#endif
