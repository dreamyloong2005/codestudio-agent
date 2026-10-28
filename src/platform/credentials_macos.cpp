#ifdef __APPLE__
#include "credentials_macos.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <stdexcept>
#include <utility>

namespace codestudio::credentials_macos {
namespace {
template<class T> struct Handle {
    T value = nullptr;
    explicit Handle(T item = nullptr) : value(item) {}
    ~Handle() { if (value) CFRelease(value); }
    Handle(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    T get() const { return value; }
};
using Dictionary = Handle<CFMutableDictionaryRef>;
using Value = Handle<CFTypeRef>;
constexpr auto marker = "codestudio-native-credential-v1";
CFStringRef service(bool oauth) {
    return oauth ? CFSTR("org.codestudio.Agent.oauth") : CFSTR("org.codestudio.Agent.credentials");
}
CFStringRef description(bool oauth) {
    return oauth ? CFSTR("CodeStudio-Agent managed OAuth record") : CFSTR("CodeStudio-Agent managed model credential");
}
Handle<CFStringRef> string(std::string_view value) {
    return Handle<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.data()), value.size(), kCFStringEncodingUTF8, false));
}
Dictionary dictionary() {
    Dictionary result(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (!result.get()) throw std::runtime_error("Cannot allocate native credential query");
    return result;
}
void set(CFMutableDictionaryRef target, const void* key, const void* value) {
    if (!value) throw std::runtime_error("Cannot allocate native credential query");
    CFDictionarySetValue(target, key, value);
}
Handle<CFArrayRef> array(CFTypeRef value) {
    return Handle<CFArrayRef>(CFArrayCreate(kCFAllocatorDefault, &value, 1, &kCFTypeArrayCallBacks));
}
std::string state(OSStatus status) {
    if (status == errSecItemNotFound) return "missing";
    if (status == errSecInteractionNotAllowed) return "locked";
    return "unavailable";
}
bool noninteractive() {
    // This is process-local, never a keychain preference. Do not restore it
    // between calls: that would let another thread accidentally display UI.
    static const bool disabled = SecKeychainSetUserInteractionAllowed(false) == errSecSuccess;
    return disabled;
}
Dictionary query(std::string_view name, bool oauth) {
    auto result = dictionary();
    const auto account = string(name);
    set(result.get(), kSecClass, kSecClassGenericPassword);
    set(result.get(), kSecAttrService, service(oauth));
    set(result.get(), kSecAttrAccount, account.get());
    set(result.get(), kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    return result;
}
void ownership(CFMutableDictionaryRef target, bool oauth) {
    const Handle<CFDataRef> data(CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(marker), std::char_traits<char>::length(marker)));
    set(target, kSecAttrLabel, CFSTR("CodeStudio-Agent credential"));
    set(target, kSecAttrDescription, description(oauth));
    set(target, kSecAttrGeneric, data.get());
}
bool owned(CFDictionaryRef attributes, std::string_view name, bool oauth) {
    auto expected = query(name, oauth);
    ownership(expected.get(), oauth);
    for (const auto key : {kSecAttrService, kSecAttrAccount, kSecAttrLabel, kSecAttrDescription, kSecAttrGeneric}) {
        const auto actual = CFDictionaryGetValue(attributes, key);
        if (!actual || !CFEqual(actual, CFDictionaryGetValue(expected.get(), key))) return false;
    }
    const auto synchronizable = CFDictionaryGetValue(attributes, kSecAttrSynchronizable);
    return !synchronizable || CFEqual(synchronizable, kCFBooleanFalse);
}
struct Session {
    Handle<SecKeychainRef> keychain;
    Value records;
    CFDictionaryRef attributes = nullptr;
    std::string status = "unavailable";
    Session(std::string_view name, bool oauth, SecKeychainRef selected) {
        if (!noninteractive()) return;
        if (selected) { CFRetain(selected); keychain.value = selected; }
        else {
            const auto result = SecKeychainCopyDefault(&keychain.value);
            if (result != errSecSuccess || !keychain.get()) return;
        }
        SecKeychainStatus flags = 0;
        const auto inspected = SecKeychainGetStatus(keychain.get(), &flags);
        if (inspected != errSecSuccess) { status = state(inspected); return; }
        if (!(flags & kSecUnlockStateStatus)) { status = "locked"; return; }
        auto request = query(name, oauth);
        const auto search = array(keychain.get());
        set(request.get(), kSecMatchSearchList, search.get());
        set(request.get(), kSecMatchLimit, kSecMatchLimitAll);
        set(request.get(), kSecReturnAttributes, kCFBooleanTrue);
        set(request.get(), kSecReturnRef, kCFBooleanTrue);
        const auto found = SecItemCopyMatching(request.get(), &records.value);
        if (found != errSecSuccess) { status = state(found); return; }
        if (!records.get() || CFGetTypeID(records.get()) != CFArrayGetTypeID()) return;
        const auto matches = static_cast<CFArrayRef>(records.get());
        if (CFArrayGetCount(matches) != 1) return;
        const auto first = CFArrayGetValueAtIndex(matches, 0);
        if (!first || CFGetTypeID(first) != CFDictionaryGetTypeID()) return;
        attributes = static_cast<CFDictionaryRef>(first);
        const auto item = CFDictionaryGetValue(attributes, kSecValueRef);
        if (!owned(attributes, name, oauth) || !item || CFGetTypeID(item) != SecKeychainItemGetTypeID()) return;
        status = "available";
    }
    Dictionary exact(std::string_view name, bool oauth) const {
        auto result = query(name, oauth);
        ownership(result.get(), oauth);
        const auto items = array(CFDictionaryGetValue(attributes, kSecValueRef));
        const auto search = array(keychain.get());
        set(result.get(), kSecMatchItemList, items.get());
        set(result.get(), kSecMatchSearchList, search.get());
        return result;
    }
};
}

CredentialValue read(std::string_view name, bool oauth, SecKeychainRef keychain) {
    Session session(name, oauth, keychain);
    if (session.status != "available") return {session.status, {}};
    auto request = session.exact(name, oauth);
    set(request.get(), kSecReturnData, kCFBooleanTrue);
    Value data;
    const auto result = SecItemCopyMatching(request.get(), &data.value);
    if (result != errSecSuccess) return {state(result), {}};
    if (!data.get() || CFGetTypeID(data.get()) != CFDataGetTypeID()) return {"unavailable", {}};
    const auto bytes = static_cast<CFDataRef>(data.get());
    const auto size = CFDataGetLength(bytes);
    if (size < 1 || size > 2048) return {"unavailable", {}};
    std::string secret(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), size);
    try { validate_credential_secret(secret); } catch (...) { return {"unavailable", {}}; }
    return {"available", std::move(secret)};
}
std::string write(std::string_view name, std::string_view secret, bool oauth, SecKeychainRef keychain) {
    Session session(name, oauth, keychain);
    if (session.status != "available" && session.status != "missing") return session.status;
    const Handle<CFDataRef> data(CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(secret.data()), secret.size()));
    if (session.status == "available") {
        auto request = session.exact(name, oauth);
        auto changes = dictionary();
        set(changes.get(), kSecValueData, data.get());
        const auto result = SecItemUpdate(request.get(), changes.get());
        return result == errSecSuccess ? "available" :
            result == errSecItemNotFound ? "unavailable" : state(result);
    }
    auto record = query(name, oauth);
    ownership(record.get(), oauth);
    set(record.get(), kSecUseKeychain, session.keychain.get());
    set(record.get(), kSecValueData, data.get());
    // Add refuses an item created concurrently; never replace an uninspected
    // record after a missing lookup.
    const auto result = SecItemAdd(record.get(), nullptr);
    return result == errSecSuccess ? "available" :
            result == errSecItemNotFound ? "unavailable" : state(result);
}
std::string erase(std::string_view name, bool oauth, SecKeychainRef keychain) {
    Session session(name, oauth, keychain);
    if (session.status != "available") return session.status;
    auto request = session.exact(name, oauth);
    const auto result = SecItemDelete(request.get());
    return result == errSecSuccess ? "deleted" : state(result);
}
}
#endif
