#include "codestudio/credentials.hpp"

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("credentials: ") + message);
}
template<class Function> void rejects(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
void set_environment(const std::string& name, const std::optional<std::string>& value) {
#ifdef _WIN32
    if (_putenv_s(name.c_str(), value ? value->c_str() : "") != 0)
        throw std::runtime_error("Cannot update isolated credential test environment");
#else
    const auto result = value ? setenv(name.c_str(), value->c_str(), 1) : unsetenv(name.c_str());
    if (result != 0) throw std::runtime_error("Cannot update isolated credential test environment");
#endif
}
struct EnvironmentRestore {
    std::string name;
    std::optional<std::string> previous;
    explicit EnvironmentRestore(std::string key) : name(std::move(key)) {
        if (const auto* value = std::getenv(name.c_str())) previous = value;
    }
    ~EnvironmentRestore() { try { set_environment(name, previous); } catch (...) {} }
};
}

void run_credentials_tests() {
    using namespace codestudio;
    for (const auto value : {"env:CODESTUDIO_API_KEY", "env:_TEST_2", "system:Model.default-2"}) {
        const auto reference = parse_credential_reference(value);
        check(reference.text() == value, "canonical reference did not round-trip");
    }
    for (const auto& value : {std::string{}, std::string("ENV:KEY"), std::string("env:"),
                             std::string("system:"), std::string("plain:token"), std::string("env:2KEY"),
                             std::string("env:KEY-X"), std::string("env:KEY=value"), std::string("env:KEY\n"),
                             std::string("system:../id"), std::string("system:_id"), std::string("system:-id"),
                             std::string("system:id/id"), std::string("system:id:other"),
                             std::string("system:id\0suffix", 16), std::string("env:") + std::string(129, 'A'),
                             std::string("system:") + std::string(129, 'a')}) {
        rejects([&] { (void)parse_credential_reference(value); }, "invalid credential reference accepted");
    }
    check(parse_credential_reference("env:" + std::string(128, 'A')).name.size() == 128,
          "maximum environment name rejected");
    check(parse_credential_reference("system:" + std::string(128, 'a')).name.size() == 128,
          "maximum system name rejected");
    validate_credential_secret("a");
    validate_credential_secret(std::string(2048, '~'));
    for (const auto& secret : {std::string{}, std::string(2049, 'a'), std::string("key with spaces"),
                              std::string("token\r\ninjected"), std::string("token\0suffix", 12),
                              std::string("token\tvalue"), std::string(1, '\x7f'), std::string(1, '\x80')}) {
        rejects([&] { validate_credential_secret(secret); }, "unsafe credential value accepted");
    }
    const EnvironmentRestore restore("CODESTUDIO_NATIVE_TEST_CREDENTIAL_ONLY");
    const auto reference = parse_credential_reference("env:" + restore.name);
    set_environment(restore.name, std::nullopt);
    auto result = read_credential(reference);
    check(result.state == "missing" && result.secret.empty(), "absent environment credential was available");
    set_environment(restore.name, "native-test-credential-a");
    result = read_credential(reference);
    check(result.state == "available" && result.secret == "native-test-credential-a", "environment read failed");
    set_environment(restore.name, "native-test-credential-b");
    result = read_credential(reference);
    check(result.state == "available" && result.secret == "native-test-credential-b", "read reused stale environment value");
    set_environment(restore.name, "unsafe\nheader");
    result = read_credential(reference);
    check(result.state == "unavailable" && result.secret.empty(), "invalid environment value escaped as a credential");
#ifdef _WIN32
    check(system_credential_backend() == "windows-credential-manager", "incorrect native credential backend");
#elif defined(__linux__)
    check(system_credential_backend() == "linux-secret-service", "incorrect native credential backend");
#else
    check(system_credential_backend() == "unsupported", "unsupported platform claimed native credentials");
#endif
}
