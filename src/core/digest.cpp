#include "codestudio/digest.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <openssl/evp.h>
#endif

namespace codestudio {
std::string sha256_bytes(std::string_view bytes) {
#ifdef _WIN32
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE handle = nullptr;
        ~Hash() {
            if (handle) BCryptDestroyHash(handle);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } hash;
    if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(hash.algorithm, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize SHA256 provider");
    while (!bytes.empty()) {
        const auto count = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), 65536));
        if (BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), count, 0) < 0)
            throw std::runtime_error("Cannot update SHA256 digest");
        bytes.remove_prefix(count);
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("Cannot finish SHA256 digest");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
#elif defined(__linux__) || defined(__APPLE__)
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes_out{};
    unsigned length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), bytes_out.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32)
        throw std::runtime_error("Cannot compute SHA256 digest");
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned index = 0; index < length; ++index) {
        result.push_back(digits[bytes_out[index] >> 4]);
        result.push_back(digits[bytes_out[index] & 15]);
    }
    return result;
#else
    (void)bytes;
    throw std::runtime_error("SHA256 is unavailable on this platform");
#endif
}
}
