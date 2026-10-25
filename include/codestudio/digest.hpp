#pragma once
#include <string>
#include <string_view>

namespace codestudio {
// Lowercase SHA-256 of the exact bytes, using the Windows cryptographic provider.
// No filesystem access. Unsupported platforms fail explicitly.
std::string sha256_bytes(std::string_view bytes);
}
