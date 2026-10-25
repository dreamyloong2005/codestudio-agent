#include "oauth_internal.hpp"
#include "transport.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace codestudio::oauth_internal {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("MCP OAuth: ") + message); }
void require(bool value, const char* message) { if (!value) fail(message); }
void check(Deadline deadline, const std::atomic_bool& cancelled) {
    require(!cancelled.load(), "operation cancelled");
    require(std::chrono::steady_clock::now() < deadline, "operation timed out");
}
Url url(const std::string& value, bool loopback) {
    try { mcp_transport::validate_http_endpoint(value, loopback); }
    catch (...) { fail("invalid HTTPS endpoint or explicit loopback URL"); }
    Url result;
    result.url = value;
    result.secure = value.starts_with("https://");
    const auto begin = result.secure ? 8u : 7u;
    const auto slash = value.find('/', begin);
    result.origin = value.substr(0, slash);
    result.path = slash == std::string::npos ? "/" : value.substr(slash);
    auto authority = value.substr(begin, slash == std::string::npos ? slash : slash - begin);
    std::string port;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        result.host = authority.substr(1, end - 1);
        if (end + 1 < authority.size()) port = authority.substr(end + 2);
    } else {
        const auto colon = authority.find(':');
        result.host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    result.port = port.empty() ? (result.secure ? 443 : 80) : static_cast<unsigned short>(std::stoi(port));
    result.loopback = result.host == "127.0.0.1" || result.host == "::1";
    return result;
}
namespace {
constexpr std::size_t limit = 1024 * 1024, headers_limit = 64 * 1024;
int remaining(Deadline deadline, const std::atomic_bool& cancelled) {
    check(deadline, cancelled);
    return static_cast<int>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count(), 1, INT_MAX));
}
struct Transfer {
    Response response;
    std::size_t header_bytes = 0;
    Deadline deadline;
    const std::atomic_bool& cancelled;
    std::exception_ptr error;
    void header(std::string_view value) {
        check(deadline, cancelled);
        require(value.size() <= headers_limit - header_bytes, "response headers exceed limit");
        header_bytes += value.size();
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
        if (value.starts_with("HTTP/")) {
            const auto start = value.find(' ');
            require(start != std::string_view::npos && value.size() > start + 3, "invalid HTTP response");
            unsigned status = 0;
            const auto parsed = std::from_chars(value.data() + start + 1, value.data() + start + 4, status);
            require(parsed.ec == std::errc{}, "invalid HTTP status");
            response.status = status;
            response.headers.clear();
            return;
        }
        if (value.empty()) return;
        const auto colon = value.find(':');
        require(colon != std::string_view::npos && value.front() != ' ' && value.front() != '\t', "invalid HTTP header");
        std::string key(value.substr(0, colon));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; });
        auto body = value.substr(colon + 1);
        while (!body.empty() && (body.front() == ' ' || body.front() == '\t')) body.remove_prefix(1);
        while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.remove_suffix(1);
        if (key == "content-type" || key == "www-authenticate") {
            require(!response.headers.contains(key), "ambiguous HTTP authorization headers");
            response.headers.emplace(std::move(key), body);
        }
    }
    void body(std::string_view value) {
        check(deadline, cancelled);
        require(value.size() <= limit - response.body.size(), "response exceeds limit");
        response.body.append(value);
    }
};
#ifdef _WIN32
std::wstring wide(std::string_view text) { return {text.begin(), text.end()}; }
struct Handle {
    HINTERNET value;
    explicit Handle(HINTERNET p) : value(p) { require(p != nullptr, "HTTP initialization failed"); }
    ~Handle() { WinHttpCloseHandle(value); }
};
#else
std::size_t header_callback(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& state = *static_cast<Transfer*>(context);
    if (size && count > SIZE_MAX / size) return 0;
    try { state.header(std::string_view(data, size * count)); }
    catch (...) { state.error = std::current_exception(); return 0; }
    return size * count;
}
std::size_t body_callback(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& state = *static_cast<Transfer*>(context);
    if (size && count > SIZE_MAX / size) return 0;
    try { state.body(std::string_view(data, size * count)); }
    catch (...) { state.error = std::current_exception(); return 0; }
    return size * count;
}
int progress_callback(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    const auto& state = *static_cast<Transfer*>(context);
    return state.cancelled.load() || std::chrono::steady_clock::now() >= state.deadline;
}
#endif
}
Response request(const std::string& endpoint, bool loopback, const std::string& body,
                 Deadline deadline, const std::atomic_bool& cancelled) {
    const auto target = url(endpoint, loopback);
    require(body.size() <= 16384, "request exceeds limit");
    const auto timeout = remaining(deadline, cancelled);
    Transfer state{{}, 0, deadline, cancelled, {}};
#ifdef _WIN32
    Handle session(WinHttpOpen(L"CodeStudio-Agent-OAuth/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    require(WinHttpSetTimeouts(session.value, timeout, timeout, timeout, timeout), "HTTP timeout setup failed");
    DWORD maximum_headers = headers_limit;
    require(WinHttpSetOption(session.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &maximum_headers, sizeof maximum_headers), "HTTP header limit setup failed");
    Handle connection(WinHttpConnect(session.value, wide(target.host).c_str(), target.port, 0));
    const auto raw = WinHttpOpenRequest(connection.value, body.empty() ? L"GET" : L"POST", wide(target.path).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, target.secure ? WINHTTP_FLAG_SECURE : 0);
    require(raw != nullptr, "HTTP request initialization failed");
    struct RequestGuard {
        std::atomic<HINTERNET> value;
        std::jthread watcher;
        explicit RequestGuard(HINTERNET p) : value(p) {}
        ~RequestGuard() {
            watcher.request_stop();
            if (watcher.joinable()) watcher.join();
            if (const auto p = value.exchange(nullptr)) WinHttpCloseHandle(p);
        }
    } guard(raw);
    DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    require(WinHttpSetOption(raw, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof disable) &&
            WinHttpSetOption(raw, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof autologon), "HTTP credential policy setup failed");
    guard.watcher = std::jthread([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (cancelled.load() || std::chrono::steady_clock::now() >= deadline) {
                if (const auto p = guard.value.exchange(nullptr)) WinHttpCloseHandle(p);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const auto network = [&] { check(deadline, cancelled); fail("HTTP exchange failed; no request retry"); };
    const auto headers = body.empty() ? L"Accept: application/json\r\n" :
        L"Accept: application/json\r\nContent-Type: application/x-www-form-urlencoded\r\n";
    if (!WinHttpSendRequest(raw, headers, static_cast<DWORD>(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) || !WinHttpReceiveResponse(raw, nullptr)) network();
    DWORD bytes = 0;
    WinHttpQueryHeaders(raw, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes <= headers_limit * sizeof(wchar_t), "invalid HTTP response headers");
    std::wstring native(bytes / sizeof(wchar_t), '\0');
    if (!WinHttpQueryHeaders(raw, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, native.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) network();
    std::string ascii;
    for (const auto c : native) { if (!c) break; require(c < 128, "unsupported HTTP header character"); ascii.push_back(static_cast<char>(c)); }
    for (std::size_t pos = 0; pos < ascii.size();) {
        const auto end = ascii.find('\n', pos);
        const auto length = end == std::string::npos ? ascii.size() - pos : end - pos + 1;
        state.header(std::string_view(ascii).substr(pos, length)); pos += length;
    }
    std::array<char, 16384> buffer{};
    while (true) {
        check(deadline, cancelled);
        DWORD received = 0;
        if (!WinHttpReadData(raw, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) network();
        if (!received) break;
        state.body(std::string_view(buffer.data(), received));
    }
#else
    static const bool initialized = [] { require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK, "curl initialization failed"); return true; }();
    (void)initialized;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    require(handle != nullptr, "HTTP initialization failed");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    for (const auto value : {"Accept: application/json", "Content-Type: application/x-www-form-urlencoded"}) {
        auto* added = curl_slist_append(headers.get(), value);
        require(added != nullptr, "HTTP headers allocation failed"); headers.release(); headers.reset(added);
    }
    const auto set = [&](CURLoption option, auto value) { require(curl_easy_setopt(handle.get(), option, value) == CURLE_OK, "HTTP setup failed"); };
    set(CURLOPT_URL, endpoint.c_str());
    set(CURLOPT_HTTPHEADER, headers.get());
    if (!body.empty()) { set(CURLOPT_POST, 1L); set(CURLOPT_POSTFIELDS, body.data()); set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())); }
    set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    set(CURLOPT_FRESH_CONNECT, 1L); set(CURLOPT_FORBID_REUSE, 1L);
    set(CURLOPT_FOLLOWLOCATION, 0L); set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_PROTOCOLS_STR, target.secure ? "https" : "http");
    set(CURLOPT_TIMEOUT_MS, static_cast<long>(timeout)); set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout));
    set(CURLOPT_NOSIGNAL, 1L); set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED)); set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
    set(CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_NONE)); set(CURLOPT_UNRESTRICTED_AUTH, 0L); set(CURLOPT_PROXY, "");
    set(CURLOPT_HEADERFUNCTION, header_callback); set(CURLOPT_HEADERDATA, &state);
    set(CURLOPT_WRITEFUNCTION, body_callback); set(CURLOPT_WRITEDATA, &state);
    set(CURLOPT_NOPROGRESS, 0L); set(CURLOPT_XFERINFOFUNCTION, progress_callback); set(CURLOPT_XFERINFODATA, &state);
    const auto code = curl_easy_perform(handle.get());
    if (state.error) std::rethrow_exception(state.error);
    check(deadline, cancelled);
    require(code == CURLE_OK, "HTTP exchange failed; no request retry");
#endif
    check(deadline, cancelled);
    require(state.response.status >= 200 && state.response.status < 600, "invalid HTTP status");
    require(state.response.status < 300 || state.response.status >= 400, "HTTP redirects are forbidden");
    return std::move(state.response);
}
}
