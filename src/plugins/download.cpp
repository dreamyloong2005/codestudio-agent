#include "internal.hpp"
#include "../mcp/transport.hpp"
#include <array>
#include <memory>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace codestudio::plugin_detail {
namespace {
using Clock = std::chrono::steady_clock;
[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("Plugin download: ") + message); }
void check(const std::atomic_bool& cancelled, Clock::time_point deadline) {
    if (cancelled.load()) fail("cancelled");
    if (Clock::now() >= deadline) fail("deadline exceeded");
}
void append(std::string& result, const char* data, std::size_t count) {
    if (count > max_bundle - result.size()) fail("bundle exceeds byte limit");
    result.append(data, count);
}
#ifdef _WIN32
struct Handle {
    HINTERNET value;
    explicit Handle(HINTERNET handle) : value(handle) { if (!value) fail("cannot create HTTP handle"); }
    ~Handle() { WinHttpCloseHandle(value); }
};
#else
struct Transfer {
    const std::atomic_bool& cancelled;
    Clock::time_point deadline;
    std::string body;
    std::size_t headers = 0;
    bool exceeded = false;
};
std::size_t body(char* data, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& state = *static_cast<Transfer*>(opaque);
    if ((size && count > SIZE_MAX / size) || state.cancelled.load() || Clock::now() >= state.deadline) return 0;
    const auto bytes = size * count;
    if (bytes > max_bundle - state.body.size()) { state.exceeded = true; return 0; }
    try { state.body.append(data, bytes); } catch (...) { return 0; }
    return bytes;
}
std::size_t headers(char*, std::size_t size, std::size_t count, void* opaque) noexcept {
    auto& state = *static_cast<Transfer*>(opaque);
    if (size && count > SIZE_MAX / size) return 0;
    const auto bytes = size * count;
    if (bytes > 65536 - state.headers) { state.exceeded = true; return 0; }
    state.headers += bytes;
    return bytes;
}
int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    const auto& state = *static_cast<Transfer*>(opaque);
    return state.cancelled.load() || Clock::now() >= state.deadline ? 1 : 0;
}
#endif
}

std::string download(const std::string& url, const std::atomic_bool& cancelled, bool allow_http_loopback) {
    // Same strict ASCII URL grammar as MCP: no userinfo, query, fragment or
    // redirect. A distribution URL is data, never shell code or a credential.
    mcp_transport::validate_http_endpoint(url, allow_http_loopback);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    check(cancelled, deadline);
#ifdef _WIN32
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1); parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)) fail("invalid URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    Handle session(WinHttpOpen(L"CodeStudio-Agent/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!WinHttpSetTimeouts(session.value, 30000, 30000, 30000, 30000)) fail("cannot set HTTP timeout");
    DWORD maximum = 65536;
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &maximum, sizeof maximum)) fail("cannot bound HTTP headers");
    Handle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    const auto raw = WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!raw) fail("cannot create HTTP request");
    struct Request {
        std::atomic<HINTERNET> handle;
        std::jthread watcher;
        explicit Request(HINTERNET value) : handle(value) {}
        ~Request() {
            watcher.request_stop();
            if (watcher.joinable()) watcher.join();
            if (const auto value = handle.exchange(nullptr)) WinHttpCloseHandle(value);
        }
    } request(raw);
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(raw, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof disabled) ||
        !WinHttpSetOption(raw, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof autologon)) fail("cannot disable automatic HTTP credentials");
    request.watcher = std::jthread([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (cancelled.load() || Clock::now() >= deadline) {
                if (const auto value = request.handle.exchange(nullptr)) WinHttpCloseHandle(value);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const auto io_error = [&] { check(cancelled, deadline); fail("HTTP transport failed"); };
    if (!WinHttpSendRequest(raw, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(raw, nullptr)) io_error();
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(raw, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &status, &size, WINHTTP_NO_HEADER_INDEX)) io_error();
    if (status != 200) fail("server did not return HTTP 200; redirects are disabled");
    std::string result;
    std::array<char, 65536> buffer{};
    for (;;) {
        check(cancelled, deadline);
        DWORD received = 0;
        if (!WinHttpReadData(raw, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) io_error();
        if (!received) break;
        append(result, buffer.data(), received);
    }
    check(cancelled, deadline);
    return result;
#else
    static const bool initialized = [] { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) fail("cannot initialize HTTP transport"); return true; }();
    (void)initialized;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) fail("cannot create HTTP handle");
    Transfer transfer{cancelled, deadline, {}, 0, false};
    const auto set = [&](CURLoption key, auto value) {
        if (curl_easy_setopt(handle.get(), key, value) != CURLE_OK) fail("cannot configure HTTP transport");
    };
    set(CURLOPT_URL, url.c_str()); set(CURLOPT_HTTPGET, 1L);
    set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    set(CURLOPT_FRESH_CONNECT, 1L); set(CURLOPT_FORBID_REUSE, 1L);
    set(CURLOPT_FOLLOWLOCATION, 0L); set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_PROTOCOLS_STR, url.starts_with("https://") ? "https" : "http");
    set(CURLOPT_TIMEOUT_MS, 30000L); set(CURLOPT_CONNECTTIMEOUT_MS, 30000L); set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_PROXY, ""); set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE)); set(CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_NONE));
    set(CURLOPT_UNRESTRICTED_AUTH, 0L); set(CURLOPT_WRITEFUNCTION, body); set(CURLOPT_WRITEDATA, &transfer);
    set(CURLOPT_HEADERFUNCTION, headers); set(CURLOPT_HEADERDATA, &transfer);
    set(CURLOPT_NOPROGRESS, 0L); set(CURLOPT_XFERINFOFUNCTION, progress); set(CURLOPT_XFERINFODATA, &transfer);
    const auto code = curl_easy_perform(handle.get());
    check(cancelled, deadline);
    if (transfer.exceeded) fail("HTTP response exceeded a size limit");
    if (code != CURLE_OK) fail("HTTP transport failed");
    long status = 0;
    if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200)
        fail("server did not return HTTP 200; redirects are disabled");
    return std::move(transfer.body);
#endif
}
}
