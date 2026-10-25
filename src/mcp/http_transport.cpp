#include "transport.hpp"
#include "../provider/protocol.hpp"
#include "codestudio/credentials.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cctype>
#include <exception>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace codestudio::mcp_transport {
namespace {

constexpr std::size_t document_limit = 1024 * 1024;
constexpr std::size_t exchange_limit = 4 * document_limit;
constexpr std::size_t header_limit = 64 * 1024;
constexpr std::size_t message_limit = 128;
constexpr std::size_t identifier_limit = 1024;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("MCP HTTP: " + message);
}

// Only a network interruption may resume an established SSE stream. Protocol
// errors, HTTP errors and callback failures must not turn into hidden retries.
class NetworkFailure final : public std::runtime_error {
public:
    explicit NetworkFailure(const std::string& message) : std::runtime_error("MCP HTTP: " + message) {}
};

class RequestTimeout final : public std::runtime_error {
public:
    RequestTimeout() : std::runtime_error("MCP HTTP: request timed out") {}
};

void check_operation(Deadline deadline, const std::atomic_bool& cancelled) {
    if (cancelled.load()) fail("request cancelled");
    if (std::chrono::steady_clock::now() >= deadline) fail("request timed out");
}

int remaining_ms(Deadline deadline, const std::atomic_bool& cancelled) {
    check_operation(deadline, cancelled);
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(left, 1, INT_MAX));
}

int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool valid_ipv4(std::string_view host) {
    int groups = 0;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto group = host.substr(0, dot);
        unsigned number = 0;
        const auto parsed = std::from_chars(group.data(), group.data() + group.size(), number);
        if (group.empty() || group.size() > 3 || (group.size() > 1 && group.front() == '0') ||
            parsed.ec != std::errc{} || parsed.ptr != group.data() + group.size() || number > 255) return false;
        ++groups;
        if (dot == std::string_view::npos) break;
        host.remove_prefix(dot + 1);
        if (host.empty()) return false;
    }
    return groups == 4;
}

bool valid_ipv6(std::string_view host) {
    const auto compression = host.find("::");
    if (compression != std::string_view::npos && host.find("::", compression + 2) != std::string_view::npos) return false;
    int groups = 0;
    const auto count = [&](std::string_view part, bool allow_ipv4) {
        while (!part.empty()) {
            const auto colon = part.find(':');
            const auto group = part.substr(0, colon);
            if (group.empty()) return false;
            if (group.find('.') != std::string_view::npos) {
                if (!allow_ipv4 || colon != std::string_view::npos || !valid_ipv4(group)) return false;
                groups += 2;
            } else {
                if (group.size() > 4 || !std::all_of(group.begin(), group.end(), [](unsigned char c) { return hex_digit(c) >= 0; })) return false;
                ++groups;
            }
            if (colon == std::string_view::npos) break;
            part.remove_prefix(colon + 1);
            if (part.empty()) return false;
        }
        return true;
    };
    if (compression == std::string_view::npos) return count(host, true) && groups == 8;
    return count(host.substr(0, compression), false) && count(host.substr(compression + 2), true) && groups < 8;
}

struct Endpoint {
    std::string url, host, path;
    unsigned short port = 443;
    bool secure = true;
    bool loopback = false;
};

Endpoint parse_endpoint(const std::string& input, bool allow_http_loopback) {
    if (input.empty() || input.size() > 8192 || input.find_first_of("@?#\\") != std::string::npos ||
        std::any_of(input.begin(), input.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) {
        fail("endpoint must be an ASCII HTTP(S) URL without userinfo, query, fragment or control characters");
    }
    Endpoint out;
    std::size_t start = 8;
    if (input.starts_with("http://")) { start = 7; out.secure = false; out.port = 80; }
    else if (!input.starts_with("https://")) fail("endpoint must use HTTPS or explicitly enabled literal HTTP loopback");
    const auto slash = input.find('/', start);
    const auto authority = std::string_view(input).substr(start, slash == std::string::npos ? slash : slash - start);
    if (authority.empty()) fail("endpoint has no host");
    std::string_view port;
    bool has_port = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || !valid_ipv6(authority.substr(1, close - 1))) fail("endpoint has invalid IPv6 host");
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') fail("endpoint has invalid authority");
            port = authority.substr(close + 2);
            has_port = true;
        }
        out.loopback = out.host == "::1";
    } else {
        const auto colon = authority.find(':');
        out.host = authority.substr(0, colon);
        if (out.host.empty() || out.host.size() > 253 ||
            std::any_of(out.host.begin(), out.host.end(), [](unsigned char c) {
                return !std::isalnum(c) && c != '-' && c != '.';
            }) || out.host.front() == '.' || out.host.back() == '.' || out.host.find("..") != std::string::npos) {
            fail("endpoint has invalid host");
        }
        if (colon != std::string_view::npos) { port = authority.substr(colon + 1); has_port = true; }
        out.loopback = out.host == "127.0.0.1";
    }
    if (has_port) {
        unsigned number = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        if (port.empty() || port.size() > 5 || parsed.ec != std::errc{} ||
            parsed.ptr != port.data() + port.size() || number == 0 || number > 65535) fail("endpoint has invalid port");
        out.port = static_cast<unsigned short>(number);
    }
    if (!out.secure && (!allow_http_loopback || !out.loopback)) fail("HTTP requires explicitly enabled literal 127.0.0.1 or [::1] loopback");
    out.path = slash == std::string::npos ? "/" : input.substr(slash);
    for (std::size_t i = 0; i < out.path.size(); ++i) {
        if (out.path[i] != '%') continue;
        if (i + 2 >= out.path.size() || hex_digit(out.path[i + 1]) < 0 || hex_digit(out.path[i + 2]) < 0) fail("endpoint has malformed percent encoding");
        const auto decoded = hex_digit(out.path[i + 1]) * 16 + hex_digit(out.path[i + 2]);
        if (decoded <= 32 || decoded == 127 || decoded == '@' || decoded == '\\') fail("endpoint has unsafe percent encoding");
        i += 2;
    }
    out.url = input;
    if (slash == std::string::npos) out.url += '/';
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool equal_ascii(std::string_view first, std::string_view second) {
    return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
}

bool visible_ascii(std::string_view value) {
    return !value.empty() && value.size() <= identifier_limit &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}

struct ResponseHeaders {
    unsigned status = 0;
    std::optional<std::string> content_type, session;
};

// Keep duplicate headers visible; a map that combines their values would hide
// ambiguous session identifiers. Count all headers, including ignored fields.
class HeaderParser {
public:
    void line(std::string_view line) {
        if (line.size() > header_limit - total_) fail("response headers exceed size limit");
        total_ += line.size();
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
        if (line.starts_with("HTTP/")) {
            const auto space = line.find(' ');
            if (space == std::string_view::npos || line.size() < space + 4) fail("invalid HTTP status");
            const auto code = line.substr(space + 1, 3);
            unsigned status = 0;
            const auto parsed = std::from_chars(code.data(), code.data() + code.size(), status);
            if (parsed.ec != std::errc{} || parsed.ptr != code.data() + code.size()) fail("invalid HTTP status");
            current_ = {status, {}, {}};
            complete_ = false;
            return;
        }
        if (line.empty()) { complete_ = current_.status >= 200; return; }
        if (complete_) fail("unexpected HTTP trailers");
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || line.front() == ' ' || line.front() == '\t') fail("invalid HTTP response header");
        const auto name = line.substr(0, colon);
        const auto value = trim(line.substr(colon + 1));
        if (equal_ascii(name, "content-type")) {
            if (current_.content_type || value.size() > 256) fail("invalid or duplicate Content-Type header");
            current_.content_type = value;
        } else if (equal_ascii(name, "mcp-session-id")) {
            if (current_.session || !visible_ascii(value)) fail("invalid or duplicate session header");
            current_.session = value;
        }
    }
    const ResponseHeaders& headers() const { return current_; }
    bool complete() const { return complete_; }
private:
    ResponseHeaders current_;
    std::size_t total_ = 0;
    bool complete_ = false;
};

using HeadersConsumer = std::function<void(const ResponseHeaders&)>;
using BodyConsumer = std::function<bool(std::string_view)>;

#ifdef _WIN32
std::wstring wide_ascii(std::string_view input) {
    return std::wstring(input.begin(), input.end());
}

struct HttpHandle {
    HINTERNET value;
    explicit HttpHandle(HINTERNET handle) : value(handle) { if (!value) fail("could not create HTTP handle"); }
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

void native_request(const Endpoint& endpoint, std::string_view method, const std::vector<std::string>& headers,
                    const std::string& body, Deadline deadline, const std::atomic_bool& cancelled,
                    const HeadersConsumer& on_headers, const BodyConsumer& on_body,
                    const std::function<void()>& on_dispatch = {}) {
    const auto timeout = remaining_ms(deadline, cancelled);
    HttpHandle session(WinHttpOpen(L"CodeStudio-Agent/0.1", endpoint.secure && !endpoint.loopback ?
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!WinHttpSetTimeouts(session.value, timeout, timeout, timeout, timeout)) fail("could not configure HTTP timeouts");
    DWORD maximum_headers = static_cast<DWORD>(header_limit);
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &maximum_headers, sizeof(maximum_headers))) fail("could not configure header limit");
    HttpHandle connection(WinHttpConnect(session.value, wide_ascii(endpoint.host).c_str(), endpoint.port, 0));
    const auto raw = WinHttpOpenRequest(connection.value, wide_ascii(method).c_str(), wide_ascii(endpoint.path).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, endpoint.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!raw) fail("could not create HTTP request");
    struct Guard {
        std::atomic<HINTERNET> handle;
        std::jthread watcher;
        explicit Guard(HINTERNET h) : handle(h) {}
        ~Guard() {
            watcher.request_stop();
            if (watcher.joinable()) watcher.join();
            if (const auto h = handle.exchange(nullptr)) WinHttpCloseHandle(h);
        }
    } guard(raw);
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(raw, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(raw, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon))) fail("could not disable automatic HTTP credentials and redirects");
    guard.watcher = std::jthread([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (cancelled.load() || std::chrono::steady_clock::now() >= deadline) {
                if (const auto h = guard.handle.exchange(nullptr)) WinHttpCloseHandle(h);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const auto io_failure = [&]() {
        const auto error = GetLastError();
        check_operation(deadline, cancelled);
        if (error == ERROR_WINHTTP_TIMEOUT) throw RequestTimeout();
        throw NetworkFailure("HTTP connection interrupted");
    };
    std::string joined;
    for (const auto& header : headers) joined += header + "\r\n";
    const auto native_headers = wide_ascii(joined);
    check_operation(deadline, cancelled);
    if (on_dispatch) on_dispatch();
    if (!WinHttpSendRequest(raw, native_headers.c_str(), static_cast<DWORD>(native_headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0)) io_failure();
    if (!WinHttpReceiveResponse(raw, nullptr)) io_failure();
    check_operation(deadline, cancelled);
    DWORD bytes = 0;
    WinHttpQueryHeaders(raw, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > header_limit * sizeof(wchar_t)) fail("response headers exceed size limit");
    std::wstring raw_headers(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(raw, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        raw_headers.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) io_failure();
    std::string ascii_headers;
    for (const auto c : raw_headers) {
        if (!c) break;
        if (c > 127) fail("response headers contain unsupported characters");
        ascii_headers.push_back(static_cast<char>(c));
    }
    HeaderParser parser;
    std::size_t start = 0;
    while (start < ascii_headers.size()) {
        const auto end = ascii_headers.find('\n', start);
        const auto count = end == std::string::npos ? ascii_headers.size() - start : end - start + 1;
        parser.line(std::string_view(ascii_headers).substr(start, count));
        start += count;
    }
    if (!parser.complete()) parser.line("");
    on_headers(parser.headers());
    std::array<char, 16384> buffer{};
    while (true) {
        check_operation(deadline, cancelled);
        DWORD received = 0;
        if (!WinHttpReadData(raw, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) io_failure();
        if (!received) break;
        if (on_body(std::string_view(buffer.data(), received))) return;
    }
    check_operation(deadline, cancelled);
}
#else
struct CurlTransfer {
    Deadline deadline;
    const std::atomic_bool& cancelled;
    const HeadersConsumer& on_headers;
    const BodyConsumer& on_body;
    HeaderParser parser;
    std::exception_ptr error;
    bool delivered_headers = false;
    bool stopped = false;
};

std::size_t receive_header(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto& state = *static_cast<CurlTransfer*>(userdata);
    if (size && count > SIZE_MAX / size) return 0;
    const auto bytes = size * count;
    try {
        check_operation(state.deadline, state.cancelled);
        state.parser.line(std::string_view(data, bytes));
        if (state.parser.complete() && !state.delivered_headers) {
            state.on_headers(state.parser.headers());
            state.delivered_headers = true;
        }
    } catch (...) { state.error = std::current_exception(); return 0; }
    return bytes;
}

std::size_t receive_body(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto& state = *static_cast<CurlTransfer*>(userdata);
    if (size && count > SIZE_MAX / size) return 0;
    const auto bytes = size * count;
    try {
        check_operation(state.deadline, state.cancelled);
        if (!state.delivered_headers) fail("response body arrived before HTTP headers");
        if (state.on_body(std::string_view(data, bytes))) { state.stopped = true; return 0; }
    } catch (...) { state.error = std::current_exception(); return 0; }
    return bytes;
}

int transfer_progress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    const auto& state = *static_cast<CurlTransfer*>(userdata);
    return state.cancelled.load() || std::chrono::steady_clock::now() >= state.deadline ? 1 : 0;
}

void native_request(const Endpoint& endpoint, std::string_view method, const std::vector<std::string>& request_headers,
                    const std::string& body, Deadline deadline, const std::atomic_bool& cancelled,
                    const HeadersConsumer& on_headers, const BodyConsumer& on_body,
                    const std::function<void()>& on_dispatch = {}) {
    const auto timeout = remaining_ms(deadline, cancelled);
    static const bool initialized = [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) fail("could not initialize curl");
        return true;
    }();
    (void)initialized;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) fail("could not create HTTP handle");
    CurlTransfer state{deadline, cancelled, on_headers, on_body, {}, {}, false, false};
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    for (const auto& value : request_headers) {
        auto* appended = curl_slist_append(headers.get(), value.c_str());
        if (!appended) fail("could not allocate HTTP headers");
        headers.release();
        headers.reset(appended);
    }
    const auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(handle.get(), option, value) != CURLE_OK) fail("could not configure HTTP transport");
    };
    set(CURLOPT_URL, endpoint.url.c_str());
    if (method == "POST") {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, body.data());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    } else if (method == "GET") set(CURLOPT_HTTPGET, 1L);
    else set(CURLOPT_CUSTOMREQUEST, "DELETE");
    set(CURLOPT_HTTPHEADER, headers.get());
    // A fresh HTTP/1.1 connection avoids HTTP/2 stream-refusal retries and
    // libcurl's stale pooled-connection retry path for a side-effecting POST.
    set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    set(CURLOPT_FRESH_CONNECT, 1L);
    set(CURLOPT_FORBID_REUSE, 1L);
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_PROTOCOLS_STR, endpoint.secure ? "https" : "http");
    set(CURLOPT_TIMEOUT_MS, static_cast<long>(timeout));
    set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout));
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
    set(CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_NONE));
    set(CURLOPT_UNRESTRICTED_AUTH, 0L);
    set(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    if (endpoint.loopback) set(CURLOPT_PROXY, "");
    set(CURLOPT_WRITEFUNCTION, receive_body);
    set(CURLOPT_WRITEDATA, &state);
    set(CURLOPT_HEADERFUNCTION, receive_header);
    set(CURLOPT_HEADERDATA, &state);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, transfer_progress);
    set(CURLOPT_XFERINFODATA, &state);
    check_operation(deadline, cancelled);
    if (on_dispatch) on_dispatch();
    const auto code = curl_easy_perform(handle.get());
    if (state.error) std::rethrow_exception(state.error);
    if (state.stopped) return;
    check_operation(deadline, cancelled);
    // Native timers have their own millisecond granularity. Their timeout may
    // occur just before the steady-clock deadline; it is still a timeout, not
    // a resumable network interruption, and requires protocol cancellation.
    if (code == CURLE_OPERATION_TIMEDOUT) throw RequestTimeout();
    if (code != CURLE_OK) throw NetworkFailure("HTTP connection interrupted");
    if (!state.delivered_headers) fail("HTTP response has no complete headers");
}
#endif

void validate_event_id(std::string_view id) {
    if (id.size() > identifier_limit || std::any_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; })) fail("invalid SSE event identifier");
    if (!id.empty() && (id.front() == ' ' || id.back() == ' ')) fail("SSE event identifier has unsupported surrounding whitespace");
    // JSON's strict UTF-8 checker rejects overlong encodings, surrogates and
    // incomplete sequences without ever printing a server-supplied identifier.
    try { (void)Json(std::string(id)).dump(); }
    catch (...) { fail("invalid UTF-8 in SSE event identifier"); }
#ifdef _WIN32
    // WinHTTP's wide-character header API has no portable UTF-8 byte contract.
    // Fail explicitly instead of silently changing a resumable stream cursor.
    if (std::any_of(id.begin(), id.end(), [](unsigned char c) { return c >= 127; })) fail("non-ASCII SSE event identifiers are unsupported by the Windows HTTP transport");
#endif
}

struct Event {
    std::optional<std::string> id;
    std::optional<std::chrono::milliseconds> retry;
    std::string name, data;
    bool has_data = false;
};

// A new parser is created for every HTTP response, so an interrupted partial
// event is discarded. Only its already dispatched predecessors can be resumed.
class SseParser {
public:
    SseParser(std::function<bool(const Event&)> dispatch,
              std::function<void(std::chrono::milliseconds)> retry_changed)
        : dispatch_(std::move(dispatch)), retry_changed_(std::move(retry_changed)) {}
    bool feed(std::string_view input) {
        for (const auto c : input) {
            if (skip_lf_) { skip_lf_ = false; if (c == '\n') continue; }
            if (++event_bytes_ > document_limit) fail("SSE event exceeds size limit");
            if (c == '\r' || c == '\n') {
                if (consume_line()) return true;
                skip_lf_ = c == '\r';
            } else line_.push_back(c);
        }
        return false;
    }
private:
    bool consume_line() {
        if (first_line_) {
            first_line_ = false;
            if (line_.starts_with("\xEF\xBB\xBF")) line_.erase(0, 3);
        }
        if (line_.empty()) {
            const bool present = event_.has_data || event_.id || event_.retry || !event_.name.empty();
            if (event_.has_data && !event_.data.empty()) event_.data.pop_back();
            const bool done = present && dispatch_(event_);
            event_ = {};
            event_bytes_ = 0;
            return done;
        }
        if (line_.front() != ':') {
            const auto colon = line_.find(':');
            const auto name = std::string_view(line_).substr(0, colon);
            auto value = colon == std::string::npos ? std::string_view{} : std::string_view(line_).substr(colon + 1);
            if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            if (name == "data") {
                event_.has_data = true;
                event_.data.append(value);
                event_.data.push_back('\n');
            } else if (name == "event") {
                event_.name = value;
            } else if (name == "id") {
                // WHATWG ignores the complete field when its value has NUL;
                // it does not clear the previous valid event identifier.
                if (value.find('\0') == std::string_view::npos) {
                    validate_event_id(value);
                    event_.id = value;
                }
            } else if (name == "retry" && !value.empty() &&
                       std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
                std::uint64_t millis = 0;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), millis);
                if (result.ec == std::errc::result_out_of_range || millis > INT_MAX) millis = INT_MAX;
                event_.retry = std::chrono::milliseconds(millis);
                // Retry is connection state, independent of event dispatch.
                // A dropped stream after this complete line still honors it.
                retry_changed_(*event_.retry);
            }
        }
        line_.clear();
        return false;
    }
    std::function<bool(const Event&)> dispatch_;
    std::function<void(std::chrono::milliseconds)> retry_changed_;
    Event event_;
    std::string line_;
    std::size_t event_bytes_ = 0;
    bool first_line_ = true, skip_lf_ = false;
};

Json document(std::string_view input) {
    try { return protocol::strict_json(input); }
    catch (...) { fail("invalid JSON-RPC response document"); }
}

class HttpTransport final : public Transport {
public:
    explicit HttpTransport(HttpOptions options)
        : endpoint_(parse_endpoint(options.url, options.allow_http_loopback)), bearer_(std::move(options.bearer)) {
        if (!bearer_.empty()) validate_credential_secret(bearer_);
    }
    ~HttpTransport() override {
        close();
        volatile char* bytes = bearer_.data();
        for (std::size_t i = 0; i < bearer_.size(); ++i) bytes[i] = '\0';
    }

    void negotiated(const std::string& version) override {
        if (!visible_ascii(version) || version.size() > 64) fail("invalid negotiated protocol version");
        if (closed_) fail("transport is closed");
        version_ = version;
    }

    void send(const Json& message, Deadline deadline, const std::atomic_bool& cancelled) override {
        if (closed_) fail("transport is closed");
        try {
            send_message(message, deadline, cancelled);
        } catch (...) { invalidate(); throw; }
    }

    void exchange(const Json& request, Deadline deadline, const std::atomic_bool& cancelled, const Receiver& receive) override {
        if (closed_) fail("transport is closed");
        const auto body = encode(request);
        const bool initializing = request.value("method", std::string{}) == "initialize";
        std::string cursor;
        std::map<std::string, std::string> delivered;
        std::chrono::milliseconds retry{0};
        std::size_t total = 0, events = 0;
        bool complete = false, recovery_safe = false, may_dispatched = false;
        const auto deliver = [&](std::string_view payload) {
            auto action = receive(document(payload));
            // Keep session state until the outer exchange has had the chance
            // to send cancellation for its still active request.
            if (action.reply) send_message(*action.reply, deadline, cancelled);
            complete = action.complete;
        };
        const auto dispatch = [&](const Event& event) {
            if (++events > message_limit) fail("SSE event count exceeds limit");
            if (event.retry) retry = *event.retry;
            const bool message = event.name.empty() || event.name == "message";
            const auto fingerprint = (event.name.empty() ? "message" : event.name) + '\n' + event.data;
            bool duplicate = false;
            if (event.id && !event.id->empty()) {
                if (const auto previous = delivered.find(*event.id); previous != delivered.end()) {
                    if (previous->second != fingerprint) fail("replayed SSE identifier changed its payload");
                    duplicate = true;
                }
            }
            if (message && event.has_data && !event.data.empty() && !duplicate) deliver(event.data);
            if (event.id) {
                cursor = *event.id;
                recovery_safe = !cursor.empty();
                if (!cursor.empty()) delivered.emplace(cursor, fingerprint);
            } else if (message && event.has_data && !event.data.empty()) {
                // Replaying past an unnumbered side request could repeat effects.
                recovery_safe = false;
            }
            return complete;
        };
        try {
            for (unsigned attempt = 0; ; ++attempt) {
                bool stream = attempt != 0, received_headers = false;
                std::string json;
                SseParser parser(dispatch, [&](std::chrono::milliseconds delay) { retry = delay; });
                auto outgoing = headers();
                if (attempt) outgoing.emplace_back("Last-Event-ID: " + cursor);
                std::function<void()> on_dispatch;
                if (!attempt) on_dispatch = [&] { may_dispatched = true; };
                try {
                    native_request(endpoint_, attempt ? "GET" : "POST", outgoing, attempt ? std::string{} : body,
                        deadline, cancelled,
                        [&](const ResponseHeaders& response) {
                            check_session(response, initializing && !attempt);
                            if (response.status != 200) fail("request requires HTTP 200");
                            if (!response.content_type) fail("response has no Content-Type");
                            const auto type = trim(std::string_view(*response.content_type).substr(0, response.content_type->find(';')));
                            stream = equal_ascii(type, "text/event-stream");
                            if (!stream && (attempt || !equal_ascii(type, "application/json"))) fail("response must be JSON or an SSE stream");
                            received_headers = true;
                        }, [&](std::string_view bytes) {
                            if (bytes.size() > exchange_limit - total) fail("exchange exceeds size limit");
                            total += bytes.size();
                            if (stream) return parser.feed(bytes);
                            if (bytes.size() > document_limit - json.size()) fail("JSON document exceeds size limit");
                            json.append(bytes);
                            return false;
                        }, on_dispatch);
                } catch (const NetworkFailure&) {
                    // Resume only the existing logical stream; never replay POST.
                    if ((!received_headers && !attempt) || !stream || !recovery_safe || cursor.empty()) throw;
                }
                if (complete) return;
                if (!stream) {
                    deliver(json);
                    if (!complete) fail("JSON response did not complete the request");
                    return;
                }
                if (!recovery_safe || cursor.empty()) fail("SSE stream ended without a complete response or resumable cursor");
                if (attempt >= 2) fail("SSE reconnect limit reached");
                const auto resume_at = std::chrono::steady_clock::now() + retry;
                while (std::chrono::steady_clock::now() < resume_at) {
                    check_operation(deadline, cancelled);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                check_operation(deadline, cancelled);
            }
        } catch (const RequestTimeout&) {
            if (may_dispatched && !initializing) cancel_request(request);
            invalidate();
            throw;
        } catch (...) {
            // Cancellation is a protocol message, not merely a closed socket.
            // Cleanup uses its own small budget after the caller's cancellation.
            if (may_dispatched && !initializing &&
                (cancelled.load() || std::chrono::steady_clock::now() >= deadline)) {
                cancel_request(request);
            }
            invalidate();
            throw;
        }
    }

    void close() noexcept override {
        if (closed_) return;
        closed_ = true;
        if (session_.empty()) return;
        try {
            const std::atomic_bool not_cancelled{false};
            native_request(endpoint_, "DELETE", headers(), {}, std::chrono::steady_clock::now() + std::chrono::milliseconds(250),
                not_cancelled, [](const ResponseHeaders&) {}, [](std::string_view) { return true; });
        } catch (...) {}
        session_.clear();
    }

private:
    void send_message(const Json& message, Deadline deadline, const std::atomic_bool& cancelled) {
        native_request(endpoint_, "POST", headers(), encode(message), deadline, cancelled,
            [&](const ResponseHeaders& response) {
                check_session(response, false);
                if (response.status != 202) fail("notification or response POST requires HTTP 202");
            }, [](std::string_view bytes) {
                if (!bytes.empty()) fail("HTTP 202 response must have an empty body");
                return false;
            });
    }

    static std::string encode(const Json& message) {
        auto body = message.dump();
        if (body.size() > document_limit) fail("request document exceeds size limit");
        return body;
    }

    std::vector<std::string> headers() const {
        std::vector<std::string> result{"Content-Type: application/json", "Accept: application/json, text/event-stream", "Expect:"};
        if (!bearer_.empty()) result.emplace_back("Authorization: Bearer " + bearer_);
        if (!session_.empty()) result.emplace_back("Mcp-Session-Id: " + session_);
        if (!version_.empty()) result.emplace_back("MCP-Protocol-Version: " + version_);
        return result;
    }

    void check_session(const ResponseHeaders& response, bool initializing) {
        if (response.status == 404) {
            invalidate();
            fail("session is no longer available; explicit reinitialization is required");
        }
        if (!response.session) return;
        if (initializing && session_.empty()) session_ = *response.session;
        else if (*response.session != session_) fail("server changed the session identifier");
    }

    void cancel_request(const Json& request) noexcept {
        try {
            if (!request.contains("id") || closed_) return;
            const auto notification = Json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
                {"params", {{"requestId", request.at("id")}, {"reason", "Client operation cancelled"}}}};
            const std::atomic_bool not_cancelled{false};
            native_request(endpoint_, "POST", headers(), encode(notification),
                std::chrono::steady_clock::now() + std::chrono::milliseconds(250), not_cancelled,
                [](const ResponseHeaders&) {}, [](std::string_view) { return true; });
        } catch (...) {}
    }

    void invalidate() noexcept { closed_ = true; session_.clear(); }

    Endpoint endpoint_;
    std::string bearer_, session_, version_;
    bool closed_ = false;
};

}

void validate_http_endpoint(const std::string& url, bool allow_http_loopback) {
    (void)parse_endpoint(url, allow_http_loopback);
}

std::unique_ptr<Transport> make_http_transport(HttpOptions options) {
    return std::make_unique<HttpTransport>(std::move(options));
}

}
