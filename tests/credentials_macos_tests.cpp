#ifdef __APPLE__
#include "../src/platform/credentials_macos.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("macOS credentials: ") + message);
}
template<class T> struct NativeHandle {
    T value = nullptr;
    ~NativeHandle() { if (value) CFRelease(value); }
};
struct PrivateKeychain {
    std::filesystem::path directory;
    SecKeychainRef value = nullptr;
    static constexpr auto password = "synthetic-native-keychain-test-password";
    PrivateKeychain() {
        auto pattern = (std::filesystem::temp_directory_path() / "codestudio-keychain-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back('\0');
        const auto created = mkdtemp(buffer.data());
        check(created, "cannot create private fixture directory");
        directory = created;
        const auto file = (directory / "fixture.keychain").string();
        const auto result = SecKeychainCreate(file.c_str(), std::char_traits<char>::length(password),
                                              password, false, nullptr, &value);
        if (result != errSecSuccess) {
            std::filesystem::remove_all(directory);
            throw std::runtime_error("macOS credentials: cannot create private fixture keychain: " + std::to_string(result));
        }
    }
    void remove() {
        if (value) {
            check(SecKeychainDelete(value) == errSecSuccess, "cannot delete private fixture keychain");
            CFRelease(value); value = nullptr;
        }
        std::filesystem::remove_all(directory);
    }
    ~PrivateKeychain() {
        if (value) { (void)SecKeychainDelete(value); CFRelease(value); }
        std::error_code error; std::filesystem::remove_all(directory, error);
    }
};
void unchanged(CFArrayRef before, SecKeychainRef default_before) {
    NativeHandle<CFArrayRef> after;
    NativeHandle<SecKeychainRef> default_after;
    check(SecKeychainCopySearchList(&after.value) == errSecSuccess && CFEqual(before, after.value),
          "private fixture changed the user's search list");
    check(SecKeychainCopyDefault(&default_after.value) == errSecSuccess && CFEqual(default_before, default_after.value),
          "private fixture changed the user's default keychain");
}
}

void run_credentials_macos_tests() {
    using namespace codestudio::credentials_macos;
    check(SecKeychainSetUserInteractionAllowed(false) == errSecSuccess, "cannot disable interactive authentication");
    NativeHandle<CFArrayRef> search;
    NativeHandle<SecKeychainRef> original_default;
    check(SecKeychainCopySearchList(&search.value) == errSecSuccess, "cannot snapshot keychain search list");
    check(SecKeychainCopyDefault(&original_default.value) == errSecSuccess, "cannot snapshot default keychain");
    PrivateKeychain fixture;
    unchanged(search.value, original_default.value);
    NativeHandle<SecKeychainRef> absent;
    const auto absent_path = (fixture.directory / "absent.keychain").string();
    check(SecKeychainOpen(absent_path.c_str(), &absent.value) == errSecSuccess, "cannot create absent store handle");
    const auto unavailable = read("native.fixture", false, absent.value);
    check(unavailable.state == "unavailable" && unavailable.secret.empty(), "absent store was not unavailable");
    check(write("native.fixture", "synthetic-missing-store", false, absent.value) == "unavailable" &&
          erase("native.fixture", false, absent.value) == "unavailable", "absent store was created implicitly");
    constexpr auto name = "native.fixture";
    auto result = read(name, false, fixture.value);
    check(result.state == "missing" && result.secret.empty(), "missing record was available");
    check(erase(name, false, fixture.value) == "missing", "deleting missing record failed");
    check(write(name, "synthetic-model-first", false, fixture.value) == "available", "native write failed");
    result = read(name, false, fixture.value);
    check(result.state == "available" && result.secret == "synthetic-model-first", "native read failed");
    check(write(name, "synthetic-model-rotated", false, fixture.value) == "available", "native rotation failed");
    result = read(name, false, fixture.value);
    check(result.state == "available" && result.secret == "synthetic-model-rotated", "native read cached old secret");
    check(write(name, "synthetic-oauth-record", true, fixture.value) == "available", "OAuth namespace write failed");
    check(read(name, false, fixture.value).secret == "synthetic-model-rotated" &&
          read(name, true, fixture.value).secret == "synthetic-oauth-record", "credential namespaces collided");

    check(SecKeychainLock(fixture.value) == errSecSuccess, "cannot lock private fixture");
    for (const bool oauth : {false, true}) {
        result = read(name, oauth, fixture.value);
        check(result.state == "locked" && result.secret.empty(), "locked read exposed a value");
        check(write(name, "synthetic-locked-replacement", oauth, fixture.value) == "locked", "locked write was allowed");
        check(erase(name, oauth, fixture.value) == "locked", "locked deletion was allowed");
    }
    check(SecKeychainUnlock(fixture.value, std::char_traits<char>::length(PrivateKeychain::password),
                           PrivateKeychain::password, true) == errSecSuccess, "cannot unlock private fixture");
    check(read(name, false, fixture.value).secret == "synthetic-model-rotated" &&
          read(name, true, fixture.value).secret == "synthetic-oauth-record", "locked operations modified records");

    // A same-service/account item with different ownership metadata must not
    // be read, replaced, or deleted. This item lives only in our private store.
    NativeHandle<SecKeychainItemRef> item;
    constexpr auto service = "org.codestudio.Agent.credentials";
    check(SecKeychainFindGenericPassword(fixture.value, std::char_traits<char>::length(service), service,
          std::char_traits<char>::length(name), name, nullptr, nullptr, &item.value) == errSecSuccess,
          "cannot locate fixture item for ownership test");
    std::string foreign = "foreign-owner";
    SecKeychainAttribute attribute{kSecDescriptionItemAttr, static_cast<UInt32>(foreign.size()), foreign.data()};
    SecKeychainAttributeList attributes{1, &attribute};
    check(SecKeychainItemModifyAttributesAndData(item.value, &attributes, 0, nullptr) == errSecSuccess,
          "cannot change fixture ownership marker");
    result = read(name, false, fixture.value);
    check(result.state == "unavailable" && result.secret.empty(), "foreign record was read");
    check(write(name, "synthetic-foreign-replacement", false, fixture.value) == "unavailable", "foreign record was replaced");
    check(erase(name, false, fixture.value) == "unavailable", "foreign record was deleted");
    UInt32 size = 0; void* bytes = nullptr;
    check(SecKeychainItemCopyContent(item.value, nullptr, nullptr, &size, &bytes) == errSecSuccess,
          "foreign fixture was unexpectedly removed");
    const std::string preserved(static_cast<const char*>(bytes), size);
    SecKeychainItemFreeContent(nullptr, bytes);
    check(preserved == "synthetic-model-rotated", "foreign record value was modified");
    check(erase(name, true, fixture.value) == "deleted" && read(name, true, fixture.value).state == "missing",
          "native deletion did not remove OAuth record");
    fixture.remove();
    unchanged(search.value, original_default.value);
}
#endif
