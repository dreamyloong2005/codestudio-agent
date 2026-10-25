#pragma once

#include <string>
#include <string_view>

namespace codestudio {
struct CredentialReference {
    std::string kind;
    std::string name;
    std::string text() const;
};

// Explicit references only: env:VARIABLE or system:identifier. Invalid input is
// rejected with a generic message; no error contains the supplied value.
CredentialReference parse_credential_reference(std::string_view reference);
void validate_credential_secret(std::string_view secret);

struct CredentialValue {
    // available, missing, locked, unavailable. Only available carries a secret.
    std::string state;
    std::string secret;
};

// A fresh lookup, without prompting, unlocking, creating a keyring, caching, or
// falling back to plaintext. The caller pins a returned value for one operation.
CredentialValue read_credential(const CredentialReference& reference);
// Current-user system store only; inputs are validated before native access.
// Write returns available/locked/unavailable; delete also returns missing.
std::string write_system_credential(std::string_view name, std::string_view secret);
std::string delete_system_credential(std::string_view name);
// Separate native namespace: OAuth records can never be read as model/Bearer
// credentials. The same size, ownership and non-interactive store rules apply.
CredentialValue read_oauth_credential(std::string_view name);
std::string write_oauth_credential(std::string_view name, std::string_view record);
std::string delete_oauth_credential(std::string_view name);
std::string system_credential_backend();
}
