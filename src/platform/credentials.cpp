#include "codestudio/credentials.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#elif defined(__linux__)
#include <libsecret/secret.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace codestudio {
namespace {
constexpr std::size_t maximum_secret_bytes = 2048;
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool valid_name(std::string_view name, bool environment) {
    if (name.empty() || name.size() > 128) return false;
    if (!(letter(name.front()) || (environment ? name.front() == '_' : digit(name.front())))) return false;
    return std::ranges::all_of(name, [environment](char c) {
        return letter(c) || digit(c) || c == '_' || (!environment && (c == '.' || c == '-'));
    });
}
bool valid_secret(std::string_view secret) {
    return !secret.empty() && secret.size() <= maximum_secret_bytes &&
        std::ranges::all_of(secret, [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
void validate_system_name(std::string_view name) {
    if (!valid_name(name, false)) throw std::invalid_argument("Invalid system credential identifier");
}

#ifdef _WIN32
constexpr auto windows_comment = L"CodeStudio-Agent managed model credential";
constexpr auto windows_username = L"CodeStudio-Agent";
std::wstring target_name(std::string_view name, bool oauth) {
    return std::wstring(oauth ? L"CodeStudio-Agent/oauth/" : L"CodeStudio-Agent/credentials/") + std::wstring(name.begin(), name.end());
}
struct CredentialDeleter { void operator()(CREDENTIALW* value) const { if (value) CredFree(value); } };
using WindowsCredential = std::unique_ptr<CREDENTIALW, CredentialDeleter>;
bool owned(const CREDENTIALW& value, const std::wstring& target) {
    return value.Type == CRED_TYPE_GENERIC && value.Flags == 0 &&
        value.Persist == CRED_PERSIST_LOCAL_MACHINE && value.AttributeCount == 0 &&
        value.TargetName && target == value.TargetName && value.Comment &&
        std::wstring_view(value.Comment) == windows_comment && value.UserName &&
        std::wstring_view(value.UserName) == windows_username &&
        (!value.TargetAlias || !*value.TargetAlias);
}
CredentialValue native_read(std::string_view name, bool oauth = false) {
    const auto target = target_name(name, oauth);
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw))
        return {GetLastError() == ERROR_NOT_FOUND ? "missing" : "unavailable", {}};
    const WindowsCredential value(raw);
    if (!owned(*value, target) || !value->CredentialBlob ||
        value->CredentialBlobSize > maximum_secret_bytes) return {"unavailable", {}};
    std::string secret(reinterpret_cast<const char*>(value->CredentialBlob), value->CredentialBlobSize);
    if (!valid_secret(secret)) return {"unavailable", {}};
    return {"available", std::move(secret)};
}
std::string native_write(std::string_view name, std::string_view secret, bool oauth = false) {
    const auto target = target_name(name, oauth);
    PCREDENTIALW raw = nullptr;
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
        const WindowsCredential previous(raw);
        if (!owned(*previous, target)) return "unavailable";
    } else if (GetLastError() != ERROR_NOT_FOUND) return "unavailable";
    CREDENTIALW value{};
    value.Type = CRED_TYPE_GENERIC;
    value.TargetName = const_cast<wchar_t*>(target.c_str());
    value.Comment = const_cast<wchar_t*>(windows_comment);
    value.UserName = const_cast<wchar_t*>(windows_username);
    value.Persist = CRED_PERSIST_LOCAL_MACHINE;
    value.CredentialBlobSize = static_cast<DWORD>(secret.size());
    value.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));
    return CredWriteW(&value, 0) ? "available" : "unavailable";
}
std::string native_delete(std::string_view name, bool oauth = false) {
    const auto target = target_name(name, oauth);
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw))
        return GetLastError() == ERROR_NOT_FOUND ? "missing" : "unavailable";
    const WindowsCredential value(raw);
    if (!owned(*value, target)) return "unavailable";
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return "deleted";
    return GetLastError() == ERROR_NOT_FOUND ? "missing" : "unavailable";
}
#elif defined(__linux__)
// Even CreateItem/Delete may return a service prompt. Override both prompt
// entry points so libsecret can never invoke UI, including races with locking.
struct CodeStudioSecretService { SecretService parent; };
struct CodeStudioSecretServiceClass { SecretServiceClass parent; };
G_DEFINE_TYPE(CodeStudioSecretService, codestudio_secret_service, SECRET_TYPE_SERVICE)

GVariant* refuse_prompt_sync(SecretService*, SecretPrompt*, GCancellable*, const GVariantType*, GError** error) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Credential interaction is disabled");
    return nullptr;
}
void refuse_prompt_async(SecretService* service, SecretPrompt*, const GVariantType*, GCancellable* cancellation,
                         GAsyncReadyCallback callback, gpointer data) {
    auto* task = g_task_new(service, cancellation, callback, data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Credential interaction is disabled");
    g_object_unref(task);
}
GVariant* refuse_prompt_finish(SecretService*, GAsyncResult* result, GError** error) {
    return static_cast<GVariant*>(g_task_propagate_pointer(G_TASK(result), error));
}
void codestudio_secret_service_class_init(CodeStudioSecretServiceClass* klass) {
    auto* service = SECRET_SERVICE_CLASS(klass);
    service->prompt_sync = refuse_prompt_sync;
    service->prompt_async = refuse_prompt_async;
    service->prompt_finish = refuse_prompt_finish;
}
void codestudio_secret_service_init(CodeStudioSecretService*) {}

template<class T> struct ObjectDeleter { void operator()(T* value) const { if (value) g_object_unref(value); } };
template<class T> using Object = std::unique_ptr<T, ObjectDeleter<T>>;
struct HashDeleter { void operator()(GHashTable* value) const { if (value) g_hash_table_unref(value); } };
using Hash = std::unique_ptr<GHashTable, HashDeleter>;
struct ListDeleter { void operator()(GList* value) const { g_list_free_full(value, g_object_unref); } };
using List = std::unique_ptr<GList, ListDeleter>;
struct ValueDeleter { void operator()(SecretValue* value) const { if (value) secret_value_unref(value); } };
using Value = std::unique_ptr<SecretValue, ValueDeleter>;
struct TextDeleter { void operator()(gchar* value) const { g_free(value); } };
using Text = std::unique_ptr<gchar, TextDeleter>;
struct Error {
    GError* value = nullptr;
    ~Error() { if (value) g_error_free(value); }
    GError** out() { return &value; }
    std::string state() const {
        return value && g_error_matches(value, SECRET_ERROR, SECRET_ERROR_IS_LOCKED) ? "locked" : "unavailable";
    }
};
class Deadline {
public:
    Deadline() : cancellation_(g_cancellable_new()), watchdog_([this](std::stop_token stop) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, stop, std::chrono::seconds(5), [] { return false; });
        if (!stop.stop_requested()) g_cancellable_cancel(cancellation_.get());
    }) {}
    ~Deadline() { watchdog_.request_stop(); changed_.notify_all(); }
    GCancellable* get() const { return cancellation_.get(); }
private:
    Object<GCancellable> cancellation_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::jthread watchdog_;
};

constexpr auto schema_name = "org.codestudio.Agent.Credential";
constexpr auto app_name = "CodeStudio-Agent";
constexpr auto item_label = "CodeStudio-Agent credential";
const SecretSchema credential_schema = {
    schema_name, SECRET_SCHEMA_NONE,
    {{"application", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"identifier", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
    0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};
Hash attributes(std::string_view name) {
    Hash result(g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free));
    g_hash_table_insert(result.get(), g_strdup("application"), g_strdup(app_name));
    const std::string stable(name);
    g_hash_table_insert(result.get(), g_strdup("identifier"), g_strdup(stable.c_str()));
    return result;
}
bool owned(SecretItem* item, std::string_view name) {
    const Hash attrs(secret_item_get_attributes(item));
    const Text label(secret_item_get_label(item));
    const Text schema(secret_item_get_schema_name(item));
    if (!attrs || !label || !schema || std::string_view(label.get()) != item_label ||
        std::string_view(schema.get()) != schema_name || g_hash_table_size(attrs.get()) != 3) return false;
    const auto equals = [&](const char* key, std::string_view expected) {
        const auto* value = static_cast<const char*>(g_hash_table_lookup(attrs.get(), key));
        return value && std::string_view(value) == expected;
    };
    return equals("application", app_name) && equals("identifier", name) && equals("xdg:schema", schema_name);
}
struct NativeSession {
    Deadline deadline;
    Error error;
    Object<SecretService> service;
    Object<SecretCollection> collection;
    Hash attrs;
    List matches;
    std::string state = "unavailable";
    explicit NativeSession(std::string_view name) : attrs(attributes(name)) {
        service.reset(secret_service_open_sync(codestudio_secret_service_get_type(), "org.freedesktop.secrets",
            SECRET_SERVICE_OPEN_SESSION, deadline.get(), error.out()));
        if (!service) { state = error.state(); return; }
        collection.reset(secret_collection_for_alias_sync(service.get(), SECRET_COLLECTION_DEFAULT,
            SECRET_COLLECTION_NONE, deadline.get(), error.out()));
        if (!collection) { state = error.state(); return; }
        if (secret_collection_get_locked(collection.get())) { state = "locked"; return; }
        matches.reset(secret_collection_search_sync(collection.get(), &credential_schema, attrs.get(),
            SECRET_SEARCH_ALL, deadline.get(), error.out()));
        if (error.value) { state = error.state(); return; }
        if (!matches) { state = "missing"; return; }
        // Ambiguous or modified records are not ours to replace or delete.
        if (matches->next || !owned(item(), name)) return;
        state = secret_item_get_locked(item()) ? "locked" : "available";
    }
    SecretItem* item() const { return matches ? SECRET_ITEM(matches->data) : nullptr; }
};
CredentialValue native_read(std::string_view name, bool oauth = false) {
    NativeSession session(oauth ? "oauth:" + std::string(name) : std::string(name));
    if (session.state != "available") return {session.state, {}};
    if (!secret_item_load_secret_sync(session.item(), session.deadline.get(), session.error.out()))
        return {session.error.state(), {}};
    const Value value(secret_item_get_secret(session.item()));
    if (!value) return {"unavailable", {}};
    gsize size = 0;
    const char* bytes = secret_value_get(value.get(), &size);
    const auto* type = secret_value_get_content_type(value.get());
    if (!bytes || size > maximum_secret_bytes || !type || std::string_view(type) != "text/plain")
        return {"unavailable", {}};
    const std::string_view secret(bytes, size);
    if (!valid_secret(secret)) return {"unavailable", {}};
    return {"available", std::string(secret)};
}
std::string native_write(std::string_view name, std::string_view secret, bool oauth = false) {
    NativeSession session(oauth ? "oauth:" + std::string(name) : std::string(name));
    if (session.state != "available" && session.state != "missing") return session.state;
    const Value value(secret_value_new(secret.data(), static_cast<gssize>(secret.size()), "text/plain"));
    if (session.item()) {
        return secret_item_set_secret_sync(session.item(), value.get(), session.deadline.get(), session.error.out())
            ? "available" : session.error.state();
    }
    const Object<SecretItem> item(secret_item_create_sync(session.collection.get(), &credential_schema,
        session.attrs.get(), item_label, value.get(), SECRET_ITEM_CREATE_REPLACE, session.deadline.get(), session.error.out()));
    return item && owned(item.get(), oauth ? "oauth:" + std::string(name) : std::string(name)) ? "available" : session.error.state();
}
std::string native_delete(std::string_view name, bool oauth = false) {
    NativeSession session(oauth ? "oauth:" + std::string(name) : std::string(name));
    if (session.state != "available") return session.state;
    return secret_item_delete_sync(session.item(), session.deadline.get(), session.error.out())
        ? "deleted" : session.error.state();
}
#else
CredentialValue native_read(std::string_view, bool = false) { return {"unavailable", {}}; }
std::string native_write(std::string_view, std::string_view, bool = false) { return "unavailable"; }
std::string native_delete(std::string_view, bool = false) { return "unavailable"; }
#endif
}

std::string CredentialReference::text() const { return kind + ":" + name; }
CredentialReference parse_credential_reference(std::string_view reference) {
    const auto separator = reference.find(':');
    if (separator == std::string_view::npos) throw std::invalid_argument("Invalid credential reference");
    const auto kind = reference.substr(0, separator), name = reference.substr(separator + 1);
    if ((kind != "env" && kind != "system") || !valid_name(name, kind == "env"))
        throw std::invalid_argument("Invalid credential reference");
    return {std::string(kind), std::string(name)};
}
void validate_credential_secret(std::string_view secret) {
    if (!valid_secret(secret)) throw std::invalid_argument("Credential must contain 1..2048 visible ASCII bytes");
}
CredentialValue read_credential(const CredentialReference& reference) {
    // Validate also aggregate-constructed references before touching a backend.
    const auto checked = parse_credential_reference(reference.text());
    if (checked.kind == "system") return native_read(checked.name);
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&raw, &length, checked.name.c_str()) != 0) {
        std::free(raw);
        return {"unavailable", {}};
    }
    const std::unique_ptr<char, decltype(&std::free)> buffer(raw, &std::free);
#else
    const char* raw = std::getenv(checked.name.c_str());
#endif
    if (!raw || !*raw) return {"missing", {}};
    const std::string_view secret(raw);
    if (!valid_secret(secret)) return {"unavailable", {}};
    return {"available", std::string(secret)};
}
std::string write_system_credential(std::string_view name, std::string_view secret) {
    validate_system_name(name);
    validate_credential_secret(secret);
    return native_write(name, secret);
}
std::string delete_system_credential(std::string_view name) {
    validate_system_name(name);
    return native_delete(name);
}
CredentialValue read_oauth_credential(std::string_view name) {
    validate_system_name(name);
    return native_read(name, true);
}
std::string write_oauth_credential(std::string_view name, std::string_view record) {
    validate_system_name(name);
    validate_credential_secret(record);
    return native_write(name, record, true);
}
std::string delete_oauth_credential(std::string_view name) {
    validate_system_name(name);
    return native_delete(name, true);
}
std::string system_credential_backend() {
#ifdef _WIN32
    return "windows-credential-manager";
#elif defined(__linux__)
    return "linux-secret-service";
#else
    return "unsupported";
#endif
}
}
