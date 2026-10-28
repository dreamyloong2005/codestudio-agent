#pragma once
#ifdef __APPLE__
#include "codestudio/credentials.hpp"
#include <Security/Security.h>

namespace codestudio::credentials_macos {
// Production callers omit keychain and use the current user's default store.
// An explicit handle allows native tests to exercise an isolated keychain
// without changing the user's default keychain or search list.
CredentialValue read(std::string_view name, bool oauth = false, SecKeychainRef keychain = nullptr);
std::string write(std::string_view name, std::string_view secret, bool oauth = false,
                  SecKeychainRef keychain = nullptr);
std::string erase(std::string_view name, bool oauth = false, SecKeychainRef keychain = nullptr);
}
#endif
