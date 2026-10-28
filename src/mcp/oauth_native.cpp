#include "oauth_internal.hpp"
#include "codestudio/digest.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <openssl/rand.h>
#ifdef __APPLE__
#include <sys/file.h>
#include <sys/stat.h>
#endif
#endif

namespace codestudio::oauth_internal {
namespace {
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
std::string base64(std::string_view text) {
    std::string result;
    unsigned accumulator = 0, bits = 0;
    for (const auto byte : text) {
        accumulator = (accumulator << 8) | static_cast<unsigned char>(byte); bits += 8;
        while (bits >= 6) { bits -= 6; result.push_back(alphabet[(accumulator >> bits) & 63]); }
    }
    if (bits) result.push_back(alphabet[(accumulator << (6 - bits)) & 63]);
    return result;
}
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
std::string decode(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (c == '%') {
            require(i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0, "invalid callback encoding");
            result.push_back(static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2]))); i += 2;
        } else result.push_back(c == '+' ? ' ' : c);
    }
    require(result.size() <= 8192 && std::ranges::all_of(result, [](unsigned char c) { return c >= 32 && c < 127; }), "invalid callback characters");
    return result;
}
#ifdef _WIN32
using Socket = SOCKET;
constexpr auto invalid_socket = INVALID_SOCKET;
void close_socket(Socket value) { if (value != invalid_socket) closesocket(value); }
void socket_start() {
    static const bool started = [] { WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "socket initialization failed"); return true; }();
    (void)started;
}
void nonblock(Socket value) { u_long mode = 1; require(ioctlsocket(value, FIONBIO, &mode) == 0, "callback socket setup failed"); }
bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
using Socket = int;
constexpr auto invalid_socket = -1;
void close_socket(Socket value) { if (value != invalid_socket) close(value); }
void socket_start() {}
void nonblock(Socket value) {
    require(fcntl(value, F_SETFL, fcntl(value, F_GETFL) | O_NONBLOCK) == 0 &&
            fcntl(value, F_SETFD, FD_CLOEXEC) == 0, "callback socket setup failed");
#ifdef __APPLE__
    const int enabled = 1;
    require(setsockopt(value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) == 0,
            "callback socket signal setup failed");
#endif
}
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif
struct SocketGuard {
    Socket value = invalid_socket;
    explicit SocketGuard(Socket value) : value(value) {}
    ~SocketGuard() { close_socket(value); }
};
void idle(Deadline deadline, const std::atomic_bool& cancelled) {
    check(deadline, cancelled); std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
}
std::string random_secret() {
    std::array<char, 32> bytes{};
#ifdef _WIN32
    require(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()), bytes.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
            "secure random generation failed");
#else
    require(RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), bytes.size()) == 1, "secure random generation failed");
#endif
    return base64({bytes.data(), bytes.size()});
}
std::string pkce(std::string_view verifier) {
    const auto digest = sha256_bytes(verifier);
    std::string bytes;
    for (std::size_t i = 0; i < digest.size(); i += 2) bytes.push_back(static_cast<char>((hex(digest[i]) << 4) | hex(digest[i + 1])));
    return base64(bytes);
}
std::string encode(std::string_view text) {
    std::string result;
    constexpr char digits[] = "0123456789ABCDEF";
    for (const unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') result.push_back(static_cast<char>(c));
        else { result.push_back('%'); result.push_back(digits[c >> 4]); result.push_back(digits[c & 15]); }
    }
    return result;
}

struct CredentialLock::Impl {
#ifdef _WIN32
    HANDLE handle = nullptr;
    bool held = false;
    ~Impl() { if (held) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
#elif defined(__APPLE__)
    int file = -1;
    ~Impl() { if (file >= 0) close(file); }
#else
    Socket socket = invalid_socket;
    ~Impl() { close_socket(socket); }
#endif
};
CredentialLock::CredentialLock(const std::string& name, Deadline deadline, const std::atomic_bool& cancelled)
    : impl_(std::make_unique<Impl>()) {
    check(deadline, cancelled);
#ifdef _WIN32
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "credential lock identity unavailable");
    struct TokenGuard { HANDLE value; ~TokenGuard() { CloseHandle(value); } } guard{token};
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    require(length > 0 && length < 65536, "credential lock identity unavailable");
    std::vector<unsigned char> data(length);
    require(GetTokenInformation(token, TokenUser, data.data(), length, &length), "credential lock identity unavailable");
    const auto sid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
    const auto key = sha256_bytes(std::string(static_cast<const char*>(sid), GetLengthSid(sid)) + ":" + name);
    const auto native = std::wstring(L"Global\\CodeStudio-Agent-OAuth-") + std::wstring(key.begin(), key.end());
    impl_->handle = CreateMutexW(nullptr, FALSE, native.c_str());
    require(impl_->handle != nullptr, "credential lock unavailable");
    while (true) {
        check(deadline, cancelled);
        const auto result = WaitForSingleObject(impl_->handle, 10);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { impl_->held = true; break; }
        require(result == WAIT_TIMEOUT, "credential lock failed");
    }
#elif defined(__APPLE__)
    // Each process opens the same private, persistent inode. Never unlink a
    // lock file on release: a waiter could otherwise lock an orphaned inode.
    // Kernel flock ownership disappears on exit/crash, and files hold no data.
    const auto length = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
    require(length > 1 && length < 65536, "credential lock directory unavailable");
    std::vector<char> directory(length);
    require(confstr(_CS_DARWIN_USER_TEMP_DIR, directory.data(), directory.size()) == length,
            "credential lock directory unavailable");
    while (directory.size() > 2 && directory[directory.size() - 2] == '/')
        directory.erase(directory.end() - 2);
    SocketGuard parent(open(directory.data(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat parent_info{};
    require(parent.value >= 0 && fstat(parent.value, &parent_info) == 0 &&
            S_ISDIR(parent_info.st_mode) && parent_info.st_uid == geteuid() &&
            (parent_info.st_mode & 077) == 0, "credential lock directory is not private");
    constexpr auto folder = "org.codestudio.Agent.oauth-locks";
    require(mkdirat(parent.value, folder, 0700) == 0 || errno == EEXIST,
            "credential lock directory unavailable");
    SocketGuard locks(openat(parent.value, folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat directory_info{};
    require(locks.value >= 0 && fstat(locks.value, &directory_info) == 0 &&
            S_ISDIR(directory_info.st_mode) && directory_info.st_uid == geteuid() &&
            (directory_info.st_mode & 07777) == 0700, "credential lock directory is not private");
    const auto key = sha256_bytes(name) + ".lock";
    impl_->file = openat(locks.value, key.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat file_info{};
    require(impl_->file >= 0 && fstat(impl_->file, &file_info) == 0 &&
            S_ISREG(file_info.st_mode) && file_info.st_uid == geteuid() &&
            (file_info.st_mode & 07777) == 0600 && file_info.st_nlink == 1 && file_info.st_size == 0,
            "credential lock file is not private");
    while (flock(impl_->file, LOCK_EX | LOCK_NB) != 0) {
        require(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR, "credential lock failed");
        idle(deadline, cancelled);
    }
    struct stat current_file{}, current_directory{};
    require(fstatat(locks.value, key.c_str(), &current_file, AT_SYMLINK_NOFOLLOW) == 0 &&
            current_file.st_dev == file_info.st_dev && current_file.st_ino == file_info.st_ino &&
            current_file.st_nlink == 1 &&
            fstatat(parent.value, folder, &current_directory, AT_SYMLINK_NOFOLLOW) == 0 &&
            current_directory.st_dev == directory_info.st_dev && current_directory.st_ino == directory_info.st_ino,
            "credential lock identity changed");
    check(deadline, cancelled);
#elif defined(__linux__)
    // Linux abstract sockets provide kernel-owned lifetime and cross-process
    // exclusion without stale filesystem lock files. This is coordination, not
    // an access-control boundary; secure-store ownership remains authoritative.
    impl_->socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(impl_->socket >= 0, "credential lock unavailable");
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    const auto key = "codestudio-oauth." + std::to_string(geteuid()) + "." + sha256_bytes(name);
    require(key.size() + 1 < sizeof address.sun_path, "credential lock identifier exceeds limit");
    std::memcpy(address.sun_path + 1, key.data(), key.size());
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + key.size() + 1);
    while (bind(impl_->socket, reinterpret_cast<sockaddr*>(&address), length) != 0) {
        require(errno == EADDRINUSE, "credential lock failed"); idle(deadline, cancelled);
    }
    check(deadline, cancelled);
#else
    fail("native OAuth credentials and locking are unsupported on this platform");
#endif
}
CredentialLock::~CredentialLock() = default;

struct Callback::Impl {
    Socket listener = invalid_socket;
    unsigned short port = 0;
    ~Impl() { close_socket(listener); }
};
Callback::Callback() : impl_(std::make_unique<Impl>()) {
    socket_start();
    impl_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(impl_->listener != invalid_socket, "callback socket unavailable");
#ifdef _WIN32
    BOOL exclusive = TRUE;
    require(setsockopt(impl_->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive) == 0,
            "callback socket ownership failed");
#endif
    nonblock(impl_->listener);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(impl_->listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 && listen(impl_->listener, 4) == 0,
            "callback listener unavailable");
#ifdef _WIN32
    int length = sizeof address;
#else
    socklen_t length = sizeof address;
#endif
    require(getsockname(impl_->listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "callback address unavailable");
    impl_->port = ntohs(address.sin_port);
}
Callback::~Callback() = default;
std::string Callback::redirect_uri() const { return "http://127.0.0.1:" + std::to_string(impl_->port) + "/oauth/callback"; }
std::string Callback::wait(const std::string& state, const std::string& issuer, bool issuer_required,
                           Deadline deadline, const std::atomic_bool& cancelled) {
    for (unsigned attempts = 0; attempts < 16;) {
        check(deadline, cancelled);
        const auto accepted = accept(impl_->listener, nullptr, nullptr);
        if (accepted == invalid_socket) { require(would_block(), "callback accept failed"); idle(deadline, cancelled); continue; }
        ++attempts;
        SocketGuard connection(accepted); nonblock(accepted);
        std::string request;
        std::array<char, 2048> buffer{};
        const auto read_deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        bool complete = false;
        while (request.size() <= 16384 && std::chrono::steady_clock::now() < read_deadline && !cancelled.load()) {
            const auto count = recv(accepted, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (count < 0) { if (!would_block()) break; idle(read_deadline, cancelled); continue; }
            if (count == 0) break;
            request.append(buffer.data(), static_cast<std::size_t>(count));
            if (request.find("\r\n\r\n") != std::string::npos) { complete = true; break; }
        }
        std::string code;
        bool denied = false;
        try {
            require(complete && request.size() <= 16384, "invalid callback request");
            const auto end = request.find("\r\n");
            const auto first = std::string_view(request).substr(0, end);
            require(first.starts_with("GET /oauth/callback?") && (first.ends_with(" HTTP/1.1") || first.ends_with(" HTTP/1.0")), "invalid callback request target");
            std::string expected_host = "127.0.0.1:" + std::to_string(impl_->port);
            std::size_t host_count = 0;
            for (std::size_t pos = end + 2; pos < request.size();) {
                const auto next = request.find("\r\n", pos); if (next == std::string::npos || next == pos) break;
                auto line = request.substr(pos, next - pos); const auto colon = line.find(':');
                require(colon != std::string::npos, "invalid callback header");
                auto key = line.substr(0, colon); std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; });
                if (key == "host") {
                    auto value = line.substr(colon + 1); while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                    require(value == expected_host && ++host_count == 1, "callback Host mismatch");
                }
                require(key != "content-length" && key != "transfer-encoding", "callback request must have no body");
                pos = next + 2;
            }
            require(host_count == 1, "callback Host missing");
            const auto query_start = first.find('?') + 1;
            auto query = first.substr(query_start, first.rfind(' ') - query_start);
            std::map<std::string, std::string> fields;
            while (!query.empty()) {
                const auto amp = query.find('&'); const auto pair = query.substr(0, amp); const auto equal = pair.find('=');
                require(equal != std::string_view::npos, "invalid callback parameter");
                require(fields.emplace(decode(pair.substr(0, equal)), decode(pair.substr(equal + 1))).second, "duplicate callback parameter");
                if (amp == std::string_view::npos) break; query.remove_prefix(amp + 1);
            }
            require(fields.contains("state") && fields.at("state") == state, "callback state mismatch");
            require((!issuer_required || fields.contains("iss")) && (!fields.contains("iss") || fields.at("iss") == issuer), "callback issuer mismatch");
            if (fields.contains("error")) { require(!fields.contains("code"), "ambiguous callback result"); denied = true; }
            else {
                require(fields.contains("code"), "callback code missing"); code = fields.at("code");
                require(!code.empty() && code.size() <= 2048 && std::ranges::all_of(code, [](unsigned char c) { return c > 32 && c < 127; }), "invalid callback code");
            }
        } catch (...) { code.clear(); denied = false; }
        const bool success = !code.empty() || denied;
        const std::string body = success ? "Authorization received. You may close this window." : "Invalid authorization callback.";
        const auto response = std::string(success ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 400 Bad Request\r\n") +
            "Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
#if defined(_WIN32) || defined(__APPLE__)
        (void)send(accepted, response.data(), static_cast<int>(response.size()), 0);
#else
        (void)send(accepted, response.data(), response.size(), MSG_NOSIGNAL);
#endif
        require(!denied, "authorization denied");
        if (!code.empty()) return code;
    }
    fail("too many invalid authorization callbacks");
}
}
