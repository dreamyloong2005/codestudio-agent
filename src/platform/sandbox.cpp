#include "codestudio/types.hpp"
#include <stdexcept>
#ifdef __linux__
#include "linux_sandbox.hpp"
#endif

namespace codestudio {
void validate_sandbox_mode(SandboxMode mode) {
    switch (mode) {
    case SandboxMode::none: return;
    case SandboxMode::appcontainer:
#ifdef _WIN32
        return;
#else
        throw std::runtime_error("AppContainer sandbox is only supported on Windows; isolation was not disabled");
#endif
    case SandboxMode::linux_namespace:
#ifdef __linux__
        LinuxSandbox::validate_available();
        return;
#else
        throw std::runtime_error("Linux namespace sandbox is only supported on Linux; isolation was not disabled");
#endif
    }
    throw std::invalid_argument("Unknown sandbox mode");
}

std::string sandbox_mode_name(SandboxMode mode) {
    switch (mode) {
    case SandboxMode::none: return "none";
    case SandboxMode::appcontainer: return "appcontainer";
    case SandboxMode::linux_namespace: return "linux-namespace";
    }
    throw std::invalid_argument("Unknown sandbox mode");
}
}
