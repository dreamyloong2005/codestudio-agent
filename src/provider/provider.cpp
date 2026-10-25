#include "protocol.hpp"
#include "codestudio/local_models.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <climits>
#include <exception>
#include <map>
#include <memory>
#include <set>
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

namespace codestudio {
namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("Model provider: " + message);
}

bool is_ascii_control(unsigned char c) { return c < 32 || c == 127; }

struct Endpoint {
    std::string host;
    std::string path;
    std::string url;
    unsigned short port = 443;
    bool secure = true;
    bool loopback = false;
};

Endpoint parse_endpoint(const ProviderConfig& config, std::string_view requested_suffix = {}) {
    const auto& input = config.base_url;
    if (input.empty() || input.find_first_of("@?#\\") != std::string::npos ||
        std::any_of(input.begin(), input.end(), [](unsigned char c) {
            return c <= 32 || c >= 127;
        })) {
        fail("base URL must be an ASCII HTTP(S) URL without userinfo, query, fragment, spaces or control characters");
    }
    Endpoint out;
    std::size_t start;
    if (input.starts_with("https://")) {
        start = 8;
    } else if (input.starts_with("http://")) {
        start = 7;
        out.secure = false;
        out.port = 80;
    } else {
        fail("base URL must use HTTPS (HTTP requires explicitly enabled literal loopback)");
    }
    const auto slash = input.find('/', start);
    const auto authority = input.substr(start, slash == std::string::npos ? slash : slash - start);
    if (authority.empty()) fail("base URL has no host");
    std::string port;
    std::string host_for_url;
    if (authority.front() == '[') {
        const auto bracket = authority.find(']');
        if (bracket == std::string::npos || bracket == 1) fail("invalid IPv6 host");
        out.host = authority.substr(1, bracket - 1);
        if (out.host.find(':') == std::string::npos ||
            std::any_of(out.host.begin(), out.host.end(), [](unsigned char c) {
                return !std::isxdigit(c) && c != ':' && c != '.';
            })) fail("invalid IPv6 host");
        host_for_url = authority.substr(0, bracket + 1);
        if (bracket + 1 < authority.size()) {
            if (authority[bracket + 1] != ':') fail("invalid URL authority");
            port = authority.substr(bracket + 2);
            if (port.empty()) fail("invalid URL port");
        }
    } else {
        const auto colon = authority.find(':');
        out.host = authority.substr(0, colon);
        if (out.host.empty() || std::any_of(out.host.begin(), out.host.end(), [](unsigned char c) {
                return !std::isalnum(c) && c != '-' && c != '.';
            })) fail("invalid URL host");
        host_for_url = out.host;
        if (colon != std::string::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) fail("invalid URL port");
        }
    }
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) fail("invalid URL port");
        const auto number = std::stoul(port);
        if (number == 0 || number > 65535) fail("invalid URL port");
        out.port = static_cast<unsigned short>(number);
    }
    out.loopback = host_for_url == "127.0.0.1" || host_for_url == "[::1]";
    if (!out.secure && (!config.allow_insecure_loopback || !out.loopback)) {
        fail("HTTP is permitted only for explicitly enabled literal 127.0.0.1 or [::1] loopback");
    }
    out.path = slash == std::string::npos ? "" : input.substr(slash);
    while (!out.path.empty() && out.path.back() == '/') out.path.pop_back();
    const std::string_view suffix = requested_suffix.empty()
        ? (config.api == ProviderApi::responses ? "/responses" :
           config.api == ProviderApi::messages ? "/messages" : "/chat/completions")
        : requested_suffix;
    for (const auto* known : {"/chat/completions", "/responses", "/messages", "/models"}) {
        if (out.path.ends_with(known) && std::string_view(known) != suffix) {
            fail("base URL endpoint does not match the selected API");
        }
    }
    if (!out.path.ends_with(suffix)) out.path += suffix;
    out.url = (out.secure ? "https://" : "http://") + host_for_url;
    if (!port.empty()) out.url += ":" + port;
    out.url += out.path;
    return out;
}

void validate_config(const ProviderConfig& config) {
    if (config.model.empty() || std::any_of(config.model.begin(), config.model.end(), is_ascii_control)) {
        fail("model must be nonempty and contain no control characters");
    }
    if (std::any_of(config.api_key.begin(), config.api_key.end(), [](unsigned char c) {
            return c <= 32 || c >= 127;
        })) fail("API key contains invalid header characters");
    if (config.timeout.count() <= 0 || config.timeout.count() > INT_MAX) {
        fail("timeout must be between 1 and 2147483647 milliseconds");
    }
    if (config.max_response_bytes == 0) fail("response size limit must be positive");
    if (config.max_output_tokens <= 0) fail("max output tokens must be positive");
}

struct HttpResponse { unsigned status; std::string body; };
using BodyConsumer = std::function<void(std::string_view)>;

bool is_event_stream(std::string_view content_type) {
    content_type = content_type.substr(0, content_type.find(';'));
    while (!content_type.empty() && std::isspace(static_cast<unsigned char>(content_type.back()))) content_type.remove_suffix(1);
    while (!content_type.empty() && std::isspace(static_cast<unsigned char>(content_type.front()))) content_type.remove_prefix(1);
    constexpr std::string_view expected = "text/event-stream";
    return content_type.size() == expected.size() &&
        std::equal(content_type.begin(), content_type.end(), expected.begin(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == b;
        });
}

std::vector<std::string> request_headers(const ProviderConfig& config) {
    std::vector<std::string> headers{"Content-Type: application/json",
        config.stream ? "Accept: text/event-stream" : "Accept: application/json"};
    if (config.api == ProviderApi::messages) {
        headers.emplace_back("anthropic-version: 2023-06-01");
        if (!config.api_key.empty()) headers.emplace_back("x-api-key: " + config.api_key);
    } else if (!config.api_key.empty()) headers.emplace_back("Authorization: Bearer " + config.api_key);
    return headers;
}

// SSE framing is independent of the provider's JSON protocol. Network chunks
// may end anywhere, including inside a UTF-8 sequence, CRLF, or the initial BOM.
class SseParser {
public:
    SseParser(protocol::StreamDecoder& decoder, std::size_t limit)
        : decoder_(decoder), limit_(std::min<std::size_t>(limit, 1024 * 1024)) {}

    void feed(std::string_view bytes) {
        for (const char byte : bytes) {
            if (skip_lf_) {
                skip_lf_ = false;
                if (byte == '\n') continue;
            }
            if (byte == '\r' || byte == '\n') {
                consume_line();
                skip_lf_ = byte == '\r';
            } else {
                if (line_.size() >= limit_) fail("SSE line exceeds configured size limit");
                line_.push_back(byte);
            }
        }
    }

    void finish() const {
        if (!line_.empty() || has_data_ || !event_.empty()) fail("stream ended within an SSE event");
    }

private:
    void consume_line() {
        if (first_line_) {
            first_line_ = false;
            if (line_.starts_with("\xEF\xBB\xBF")) line_.erase(0, 3);
        }
        if (line_.empty()) {
            if (has_data_) {
                if (!data_.empty()) data_.pop_back(); // SSE appends LF per data field.
                decoder_.event(event_, data_);
            }
            data_.clear();
            event_.clear();
            has_data_ = false;
        } else if (line_.front() != ':') {
            const auto colon = line_.find(':');
            const auto field = std::string_view(line_).substr(0, colon);
            auto value = colon == std::string::npos ? std::string_view{} : std::string_view(line_).substr(colon + 1);
            if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            if (field == "data") {
                if (value.size() >= limit_ || data_.size() > limit_ - value.size() - 1) {
                    fail("SSE event exceeds configured size limit");
                }
                data_.append(value);
                data_.push_back('\n');
                has_data_ = true;
            } else if (field == "event") {
                if (value.size() > limit_) fail("SSE event name exceeds configured size limit");
                event_.assign(value);
            }
        }
        line_.clear();
    }
    protocol::StreamDecoder& decoder_;
    std::size_t limit_;
    std::string line_, event_, data_;
    bool first_line_ = true;
    bool skip_lf_ = false;
    bool has_data_ = false;
};

#ifdef _WIN32
std::wstring wide(const std::string& input) {
    if (input.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0);
    if (!count) fail("invalid UTF-8 in HTTP configuration");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), result.data(), count);
    return result;
}

struct HttpHandle {
    HINTERNET handle;
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    explicit HttpHandle(HINTERNET h) : handle(h) { if (!handle) fail("could not create HTTP handle"); }
};

HttpResponse request(const Endpoint& endpoint, const ProviderConfig& config,
                     const std::string& body, const std::atomic_bool& cancelled,
                     const BodyConsumer& consume = {}, bool get = false) {
    if (cancelled.load()) fail("request cancelled");
    if (body.size() > MAXDWORD) fail("request exceeds transport size limit");
    HttpHandle session(WinHttpOpen(L"CodeStudio-Agent/0.1", endpoint.secure && !endpoint.loopback ?
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    const auto timeout = static_cast<int>(config.timeout.count());
    if (!WinHttpSetTimeouts(session.handle, timeout, timeout, timeout, timeout)) {
        fail("could not configure request timeout");
    }
    HttpHandle connection(WinHttpConnect(session.handle, wide(endpoint.host).c_str(), endpoint.port, 0));
    auto raw_request = WinHttpOpenRequest(connection.handle, get ? L"GET" : L"POST", wide(endpoint.path).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        endpoint.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!raw_request) fail("could not create HTTP request");
    // Closing a pending request cancels blocking WinHTTP I/O. The guard joins its
    // watcher before releasing parent handles, and owns the request exactly once.
    struct RequestGuard {
        std::atomic<HINTERNET> handle;
        std::atomic_bool deadline_reached{false};
        std::jthread watcher;
        explicit RequestGuard(HINTERNET h) : handle(h) {}
        ~RequestGuard() {
            watcher.request_stop();
            if (watcher.joinable()) watcher.join();
            if (auto h = handle.exchange(nullptr)) WinHttpCloseHandle(h);
        }
    } guard(raw_request);
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(raw_request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(raw_request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon))) {
        fail("could not disable redirects and implicit authentication");
    }
    const auto deadline = std::chrono::steady_clock::now() + config.timeout;
    guard.watcher = std::jthread([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            const bool expired = std::chrono::steady_clock::now() >= deadline;
            if (cancelled.load() || expired) {
                guard.deadline_reached.store(expired);
                if (auto h = guard.handle.exchange(nullptr)) WinHttpCloseHandle(h);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const auto check = [&]() {
        if (cancelled.load()) fail("request cancelled");
        if (guard.deadline_reached.load() || std::chrono::steady_clock::now() >= deadline) {
            fail("request timed out");
        }
    };
    const auto io_failure = [&]() {
        const auto code = GetLastError();
        check();
        if (code == ERROR_WINHTTP_TIMEOUT) fail("request timed out");
        fail("HTTP transport failed (WinHTTP code " + std::to_string(code) + ")");
    };
    std::string headers;
    for (const auto& header : request_headers(config)) headers += header + "\r\n";
    const auto native_headers = wide(headers);
    check();
    if (!WinHttpSendRequest(raw_request, native_headers.c_str(), static_cast<DWORD>(native_headers.size()),
            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) {
        io_failure();
    }
    check();
    if (!WinHttpReceiveResponse(raw_request, nullptr)) io_failure();
    check();
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(raw_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) io_failure();
    HttpResponse result{status, {}};
    const bool success = status >= 200 && status < 300;
    if (success && config.stream) {
        std::array<wchar_t, 256> content_type{};
        DWORD type_size = static_cast<DWORD>(content_type.size() * sizeof(wchar_t));
        if (!WinHttpQueryHeaders(raw_request, WINHTTP_QUERY_CONTENT_TYPE,
                WINHTTP_HEADER_NAME_BY_INDEX, content_type.data(), &type_size, WINHTTP_NO_HEADER_INDEX)) {
            fail("streaming response must have a text/event-stream Content-Type");
        }
        std::string ascii_type;
        for (const auto c : content_type) {
            if (!c) break;
            if (c > 127) fail("streaming response has an invalid Content-Type");
            ascii_type.push_back(static_cast<char>(c));
        }
        if (!is_event_stream(ascii_type)) fail("streaming response must have a text/event-stream Content-Type");
    }
    std::array<char, 16384> buffer{};
    std::size_t total = 0;
    while (true) {
        check();
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(raw_request, &available)) io_failure();
        if (!available) break;
        DWORD received = 0;
        const auto wanted = std::min(available, static_cast<DWORD>(buffer.size()));
        if (!WinHttpReadData(raw_request, buffer.data(), wanted, &received)) io_failure();
        if (!received) break;
        if (received > config.max_response_bytes - total) fail("response exceeds configured size limit");
        total += received;
        if (success && consume) consume(std::string_view(buffer.data(), received));
        else if (success) result.body.append(buffer.data(), received);
    }
    check();
    return result;
}
#else
struct CurlTransfer {
    const ProviderConfig& config;
    const std::atomic_bool& cancelled;
    const BodyConsumer& consume;
    std::string body;
    std::string content_type;
    std::exception_ptr callback_error;
    std::size_t total = 0;
    unsigned status = 0;
    bool too_large = false;
    bool invalid_content_type = false;
    bool headers_complete = false;
};

std::size_t receive_header(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto& transfer = *static_cast<CurlTransfer*>(userdata);
    if (size && count > SIZE_MAX / size) return 0;
    const auto bytes = size * count;
    try {
        auto line = std::string_view(data, bytes);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
        if (line.starts_with("HTTP/")) {
            const auto space = line.find(' ');
            if (space == std::string_view::npos) return 0;
            const auto code = line.substr(space + 1, 3);
            unsigned status = 0;
            const auto parsed = std::from_chars(code.data(), code.data() + code.size(), status);
            if (code.size() != 3 || parsed.ec != std::errc{} || parsed.ptr != code.data() + code.size()) return 0;
            transfer.status = status;
            transfer.content_type.clear();
            transfer.invalid_content_type = false;
            transfer.headers_complete = false;
        } else if (line.empty()) {
            transfer.headers_complete = true;
        } else if (!transfer.headers_complete) {
            const auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                const auto key = line.substr(0, colon);
                constexpr std::string_view expected = "content-type";
                if (key.size() == expected.size() && std::equal(key.begin(), key.end(), expected.begin(),
                    [](unsigned char a, unsigned char b) { return std::tolower(a) == b; })) {
                    if (!transfer.content_type.empty() || line.size() > 512) transfer.invalid_content_type = true;
                    else transfer.content_type.assign(line.substr(colon + 1));
                }
            }
        }
    } catch (...) { transfer.callback_error = std::current_exception(); return 0; }
    return bytes;
}

std::size_t receive_body(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto& transfer = *static_cast<CurlTransfer*>(userdata);
    if (transfer.cancelled.load()) return 0;
    if (size && count > SIZE_MAX / size) { transfer.too_large = true; return 0; }
    const auto bytes = size * count;
    if (bytes > transfer.config.max_response_bytes - transfer.total) {
        transfer.too_large = true;
        return 0;
    }
    transfer.total += bytes;
    if (transfer.status >= 200 && transfer.status < 300) {
        if (transfer.config.stream && (transfer.invalid_content_type || !is_event_stream(transfer.content_type))) {
            transfer.invalid_content_type = true;
            return 0;
        }
        try {
            if (transfer.consume) transfer.consume(std::string_view(data, bytes));
            else transfer.body.append(data, bytes);
        } catch (...) { transfer.callback_error = std::current_exception(); return 0; }
    }
    return bytes;
}

int transfer_progress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<CurlTransfer*>(userdata)->cancelled.load() ? 1 : 0;
}

HttpResponse request(const Endpoint& endpoint, const ProviderConfig& config,
                     const std::string& body, const std::atomic_bool& cancelled,
                     const BodyConsumer& consume = {}, bool get = false) {
    if (cancelled.load()) fail("request cancelled");
    static const int initialization = [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) fail("could not initialize curl");
        return 0;
    }();
    (void)initialization;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) fail("could not create HTTP handle");
    CurlTransfer transfer{config, cancelled, consume, {}, {}, {}};
    curl_slist* raw_headers = nullptr;
    const auto add_header = [&](const std::string& value) {
        auto* next = curl_slist_append(raw_headers, value.c_str());
        if (!next) { curl_slist_free_all(raw_headers); fail("could not allocate HTTP headers"); }
        raw_headers = next;
    };
    for (const auto& header : request_headers(config)) add_header(header);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    const auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(handle.get(), option, value) != CURLE_OK) fail("could not configure HTTP transport");
    };
    set(CURLOPT_URL, endpoint.url.c_str());
    if (get) set(CURLOPT_HTTPGET, 1L);
    else {
        set(CURLOPT_POST, 1L);
        set(CURLOPT_POSTFIELDS, body.data());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_PROTOCOLS_STR, endpoint.secure ? "https" : "http");
    set(CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout.count()));
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    if (!endpoint.secure || endpoint.loopback) set(CURLOPT_PROXY, "");
    set(CURLOPT_WRITEFUNCTION, receive_body);
    set(CURLOPT_WRITEDATA, &transfer);
    set(CURLOPT_HEADERFUNCTION, receive_header);
    set(CURLOPT_HEADERDATA, &transfer);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, transfer_progress);
    set(CURLOPT_XFERINFODATA, &transfer);
    const auto code = curl_easy_perform(handle.get());
    if (cancelled.load()) fail("request cancelled");
    if (transfer.too_large) fail("response exceeds configured size limit");
    if (transfer.callback_error) std::rethrow_exception(transfer.callback_error);
    if (transfer.invalid_content_type || (config.stream && transfer.status >= 200 && transfer.status < 300 &&
        !is_event_stream(transfer.content_type))) fail("streaming response must have a text/event-stream Content-Type");
    if (code == CURLE_OPERATION_TIMEDOUT) fail("request timed out");
    if (code != CURLE_OK) fail("HTTP transport failed (curl code " + std::to_string(code) + ")");
    long status = 0;
    if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) fail("could not read HTTP status");
    return {static_cast<unsigned>(status), std::move(transfer.body)};
}
#endif

bool valid_function_name(const std::string& name) {
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

Json parse_json(std::string_view text) {
    // Bound nesting before entering the recursive JSON parser. Duplicate object
    // keys are ambiguous for executable arguments and are rejected everywhere.
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const auto c : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 128) return Json(Json::value_t::discarded); }
        else if (c == ']' || c == '}') { if (depth) --depth; }
    }
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second) duplicate = true;
        return true;
    };
    auto result = Json::parse(text, callback, false);
    return duplicate ? Json(Json::value_t::discarded) : result;
}

ModelReply parse_reply(const Json& document, const Json& history) {
    if (document.is_discarded() || !document.is_object()) fail("response is not a valid JSON object");
    if (!document.contains("choices") || !document["choices"].is_array() || document["choices"].size() != 1) {
        fail("response must contain exactly one choice");
    }
    const auto& choice = document["choices"][0];
    if (!choice.is_object() || !choice.contains("finish_reason") || !choice["finish_reason"].is_string()) {
        fail("response is missing a complete finish_reason");
    }
    const auto finish = choice["finish_reason"].get<std::string>();
    if (finish == "length") fail("model output was truncated; increase the output budget before retrying");
    if (finish == "content_filter") fail("model output was blocked by a content filter");
    if (finish != "stop" && finish != "tool_calls") fail("unsupported or incomplete model finish_reason");
    if (!choice.contains("message") || !choice["message"].is_object()) fail("response is missing its assistant message");
    const auto& message = choice["message"];
    if (!message.contains("role") || message["role"] != "assistant") fail("response message role must be assistant");
    if (message.contains("refusal") && !message["refusal"].is_null() && message["refusal"] != "") {
        fail("model refused the request");
    }
    if (message.contains("function_call") && !message["function_call"].is_null()) {
        fail("legacy function_call is unsupported; configure a tool_calls-compatible model");
    }
    ModelReply result;
    if (message.contains("content") && !message["content"].is_null()) {
        if (!message["content"].is_string()) fail("assistant content must be a string or null");
        result.text = message["content"].get<std::string>();
    }
    result.message = {{"role", "assistant"}, {"content", result.text.empty() ? Json(nullptr) : Json(result.text)}};
    std::set<std::string> ids;
    for (const auto& item : history) {
        if (!item.is_object() || !item.contains("tool_calls") || !item["tool_calls"].is_array()) continue;
        for (const auto& call : item["tool_calls"]) {
            if (call.is_object() && call.contains("id") && call["id"].is_string()) ids.insert(call["id"].get<std::string>());
        }
    }
    if (message.contains("tool_calls") && !message["tool_calls"].is_null()) {
        if (!message["tool_calls"].is_array()) fail("tool_calls must be an array");
        if (message["tool_calls"].size() > 64) fail("response contains too many tool calls");
        Json normalized = Json::array();
        for (const auto& call : message["tool_calls"]) {
            if (!call.is_object() || !call.contains("id") || !call["id"].is_string() ||
                !call.contains("type") || call["type"] != "function" ||
                !call.contains("function") || !call["function"].is_object()) fail("malformed tool call");
            const auto id = call["id"].get<std::string>();
            if (id.empty() || id.size() > 256 || std::any_of(id.begin(), id.end(), [](unsigned char c) {
                    return c <= 32 || c >= 127;
                }) || !ids.insert(id).second) fail("tool call IDs must be nonempty, unique ASCII identifiers");
            const auto& function = call["function"];
            if (!function.contains("name") || !function["name"].is_string() ||
                !valid_function_name(function["name"].get<std::string>()) ||
                !function.contains("arguments") || !function["arguments"].is_string()) fail("malformed tool function or arguments");
            const auto name = function["name"].get<std::string>();
            auto arguments = parse_json(function["arguments"].get<std::string>());
            if (arguments.is_discarded() || !arguments.is_object()) fail("tool arguments must be a complete JSON object");
            normalized.push_back({{"id", id}, {"type", "function"},
                                  {"function", {{"name", name}, {"arguments", arguments.dump()}}}});
            result.calls.push_back({id, name, std::move(arguments)});
        }
        if (!normalized.empty()) result.message["tool_calls"] = std::move(normalized);
    }
    if (finish == "tool_calls" && result.calls.empty()) fail("tool_calls finish_reason has no tool calls");
    if (finish == "stop" && !result.calls.empty()) fail("tool calls require a tool_calls finish_reason");
    if (result.calls.empty() && std::all_of(result.text.begin(), result.text.end(), [](unsigned char c) {
            return std::isspace(c);
        })) fail("model returned an empty final answer");
    // Retain only standard numeric accounting fields. Provider-specific metadata
    // and arbitrary response fields do not become executable history or logs.
    if (document.contains("usage") && document["usage"].is_object()) {
        for (const auto* key : {"prompt_tokens", "completion_tokens", "total_tokens"}) {
            const auto it = document["usage"].find(key);
            if (it != document["usage"].end() && it->is_number_integer() && *it >= 0) result.usage[key] = *it;
        }
    }
    return result;
}

class ChatStreamDecoder final : public protocol::StreamDecoder {
public:
    ChatStreamDecoder(const ProviderConfig& config, const Json& history)
        : config_(config), history_(history) {}

    void event(std::string_view name, std::string_view data) override {
        if (done_) fail("data arrived after the stream terminal event");
        if (!name.empty() && name != "message") fail("unsupported Chat Completions stream event");
        if (data == "[DONE]") {
            if (finish_reason_.empty()) fail("stream ended without a complete finish_reason");
            done_ = true;
            return;
        }
        const auto document = protocol::strict_json(data);
        if (!document.is_object()) fail("stream event is not a JSON object");
        if (document.contains("error") && !document["error"].is_null()) fail("model returned a streaming error");
        if (document.contains("id")) {
            if (!document["id"].is_string()) fail("stream response ID must be a string");
            const auto id = document["id"].get<std::string>();
            if (!stream_id_.empty() && stream_id_ != id) fail("conflicting stream response IDs");
            if (stream_id_.empty()) stream_id_ = id;
        }
        if (document.contains("usage") && !document["usage"].is_null()) {
            if (!document["usage"].is_object()) fail("stream usage must be an object");
            usage_ = document["usage"];
        }
        if (!document.contains("choices") || !document["choices"].is_array()) fail("stream event has no choices array");
        const auto& choices = document["choices"];
        if (choices.empty()) {
            if (!document.contains("usage") || !document["usage"].is_object()) fail("empty stream choices require usage data");
            return;
        }
        if (!finish_reason_.empty()) fail("choice data arrived after finish_reason");
        if (choices.size() != 1) fail("stream event must contain exactly one choice");
        const auto& choice = choices[0];
        if (!choice.is_object() || !choice.contains("index") || !choice["index"].is_number_integer() || choice["index"] != 0) {
            fail("stream choice index must be zero");
        }
        if (!choice.contains("delta") || !choice["delta"].is_object()) fail("stream choice has no delta object");
        const auto& delta = choice["delta"];
        if (delta.contains("role") && !delta["role"].is_null() && delta["role"] != "assistant") fail("stream message role must be assistant");
        if (delta.contains("refusal") && !delta["refusal"].is_null() && delta["refusal"] != "") fail("model refused the request");
        if (delta.contains("function_call") && !delta["function_call"].is_null()) fail("legacy streaming function_call is unsupported");
        if (delta.contains("content") && !delta["content"].is_null()) {
            if (!delta["content"].is_string()) fail("stream content delta must be a string");
            const auto text = delta["content"].get<std::string>();
            text_ += text;
            if (!text.empty() && config_.on_text_delta) {
                try { config_.on_text_delta(text); }
                catch (...) { fail("text output callback failed"); }
            }
        }
        if (delta.contains("tool_calls") && !delta["tool_calls"].is_null()) {
            if (!delta["tool_calls"].is_array() || delta["tool_calls"].size() > 64) fail("stream tool_calls must be a bounded array");
            std::set<std::size_t> event_indexes;
            for (const auto& item : delta["tool_calls"]) {
                if (!item.is_object() || !item.contains("index") || !item["index"].is_number_integer() ||
                    item["index"] < 0 || item["index"] >= 64) fail("stream tool index is invalid");
                const auto index = item["index"].get<std::size_t>();
                if (!event_indexes.insert(index).second) fail("duplicate tool index within stream event");
                auto& call = calls_[index];
                if (item.contains("type") && !item["type"].is_null() && item["type"] != "function") fail("unsupported streaming tool type");
                if (item.contains("id") && !item["id"].is_null()) append(item["id"], call.id, 256);
                if (item.contains("function") && !item["function"].is_null()) {
                    if (!item["function"].is_object()) fail("stream tool function must be an object");
                    const auto& function = item["function"];
                    if (function.contains("name") && !function["name"].is_null()) append(function["name"], call.name, 64);
                    if (function.contains("arguments") && !function["arguments"].is_null()) append(function["arguments"], call.arguments, config_.max_response_bytes);
                }
            }
        }
        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            if (!choice["finish_reason"].is_string()) fail("stream finish_reason must be a string");
            finish_reason_ = choice["finish_reason"].get<std::string>();
            if (finish_reason_ == "length") fail("model output was truncated; increase the output budget before retrying");
            if (finish_reason_ == "content_filter") fail("model output was blocked by a content filter");
            if (finish_reason_ != "stop" && finish_reason_ != "tool_calls") fail("unsupported or incomplete model finish_reason");
        }
    }

    ModelReply finish() override {
        if (!done_ || finish_reason_.empty()) fail("stream ended without its successful terminal event");
        Json message{{"role", "assistant"}, {"content", text_.empty() ? Json(nullptr) : Json(text_)}};
        if (!calls_.empty()) {
            Json calls = Json::array();
            for (const auto& [index, call] : calls_) {
                if (index != calls.size()) fail("stream tool indexes must be contiguous");
                calls.push_back({{"id", call.id}, {"type", "function"},
                    {"function", {{"name", call.name}, {"arguments", call.arguments}}}});
            }
            message["tool_calls"] = std::move(calls);
        }
        return parse_reply({{"choices", Json::array({{{"finish_reason", finish_reason_}, {"message", std::move(message)}}})},
            {"usage", usage_}}, history_);
    }
private:
    static void append(const Json& value, std::string& destination, std::size_t limit) {
        if (!value.is_string()) fail("stream tool fragment must be a string");
        const auto& fragment = value.get_ref<const std::string&>();
        if (fragment.size() > limit - destination.size()) fail("stream tool fragment exceeds configured size limit");
        destination += fragment;
    }
    struct PendingCall { std::string id, name, arguments; };
    const ProviderConfig& config_;
    const Json& history_;
    std::map<std::size_t, PendingCall> calls_;
    std::string text_, finish_reason_, stream_id_;
    Json usage_ = Json::object();
    bool done_ = false;
};

Json chat_request(const ProviderConfig& config, const Json& history, const Json& tools) {
    Json messages = Json::array();
    for (const auto& item : history) {
        if (!item.is_object()) fail("history messages must be objects");
        Json message = Json::object();
        // Other adapters store opaque continuation records in normalized history.
        // Only Chat Completions fields may cross this protocol boundary.
        for (const auto* key : {"role", "content", "tool_calls", "tool_call_id", "name"}) {
            if (item.contains(key)) message[key] = item[key];
        }
        messages.push_back(std::move(message));
    }
    Json payload{{"model", config.model}, {"messages", std::move(messages)}, {"stream", config.stream}, {"n", 1}};
    if (config.stream) payload["stream_options"] = {{"include_usage",true}};
    if (!tools.empty()) {
        payload["tools"] = tools;
        payload["tool_choice"] = "auto";
        payload["parallel_tool_calls"] = false;
    }
    return payload;
}

class OpenAICompatibleProvider final : public ModelProvider {
public:
    explicit OpenAICompatibleProvider(ProviderConfig config) : config_(std::move(config)), endpoint_(parse_endpoint(config_)) {
        validate_config(config_);
    }
    std::string name() const override { return "openai-compatible/" + provider_api_name(config_.api); }
    ModelReply complete(const Json& messages, const Json& tools, const std::atomic_bool& cancelled) override {
        if (!messages.is_array() || messages.empty() || !tools.is_array()) fail("messages and tools must be arrays, with at least one message");
        Json payload;
        std::unique_ptr<protocol::StreamDecoder> decoder;
        switch (config_.api) {
            case ProviderApi::chat_completions:
                payload = chat_request(config_, messages, tools);
                if (config_.stream) decoder = std::make_unique<ChatStreamDecoder>(config_, messages);
                break;
            case ProviderApi::responses:
                payload = protocol::responses_request(config_, messages, tools);
                if (config_.stream) decoder = protocol::responses_stream(config_, messages);
                break;
            case ProviderApi::messages:
                payload = protocol::messages_request(config_, messages, tools);
                if (config_.stream) decoder = protocol::messages_stream(config_, messages);
                break;
            default: fail("unknown provider API");
        }
        std::unique_ptr<SseParser> sse;
        BodyConsumer consume;
        if (decoder) {
            sse = std::make_unique<SseParser>(*decoder, config_.max_response_bytes);
            consume = [&](std::string_view bytes) { sse->feed(bytes); };
        }
        auto response = request(endpoint_, config_, payload.dump(), cancelled, consume);
        if (response.status < 200 || response.status >= 300) {
            // Deliberately do not include the response body, Location header, URL
            // or native textual errors: gateways can reflect credentials there.
            fail("HTTP status " + std::to_string(response.status) + " (response body omitted)");
        }
        if (decoder) {
            sse->finish();
            return decoder->finish();
        }
        const auto document = protocol::strict_json(response.body);
        switch (config_.api) {
            case ProviderApi::chat_completions: return parse_reply(document, messages);
            case ProviderApi::responses: return protocol::responses_reply(document, messages);
            case ProviderApi::messages: return protocol::messages_reply(document, messages);
        }
        fail("unknown provider API");
    }
private:
    ProviderConfig config_;
    Endpoint endpoint_;
};

constexpr const char* demo_content = "Hello from CodeStudio-Agent!\n";

ModelReply demo_call(const char* id, const char* name, Json arguments) {
    ModelReply reply;
    reply.calls.push_back({id, name, arguments});
    reply.message = {{"role", "assistant"}, {"content", nullptr}, {"tool_calls", Json::array({
        {{"id", id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", arguments.dump()}}}}
    })}};
    return reply;
}

class DemoProvider final : public ModelProvider {
public:
    std::string name() const override { return "offline-demo"; }
    ModelReply complete(const Json& messages, const Json&, const std::atomic_bool& cancelled) override {
        if (cancelled.load()) fail("request cancelled");
        if (!messages.is_array()) fail("demo history must be an array");
        const Json* write_result = nullptr;
        const Json* read_result = nullptr;
        for (const auto& item : messages) {
            if (!item.is_object() || item.value("role", "") != "tool") continue;
            if (item.value("tool_call_id", "") == "demo_write_1") write_result = &item;
            if (item.value("tool_call_id", "") == "demo_read_1") read_result = &item;
        }
        const auto parse_result = [](const Json& item) {
            if (!item.contains("content") || !item["content"].is_string()) fail("demo tool returned an invalid result");
            auto result = parse_json(item["content"].get<std::string>());
            if (!result.is_object() || !result.contains("ok") || result["ok"] != true) fail("demo tool failed; no successful completion is claimed");
            return result;
        };
        if (!write_result) return demo_call("demo_write_1", "write_file", {{"path", "CODESTUDIO_DEMO.txt"}, {"content", demo_content}});
        (void)parse_result(*write_result);
        if (!read_result) return demo_call("demo_read_1", "read_file", {{"path", "CODESTUDIO_DEMO.txt"}});
        const auto result = parse_result(*read_result);
        if (!result.contains("output") || !result["output"].is_object() ||
            !result["output"].contains("content") || result["output"]["content"] != demo_content) {
            fail("demo read-back verification failed");
        }
        ModelReply reply;
        reply.text = "Offline demo completed: wrote CODESTUDIO_DEMO.txt and verified its contents by reading it back.";
        reply.message = {{"role", "assistant"}, {"content", reply.text}};
        return reply;
    }
};
}

namespace protocol {
Json strict_json(std::string_view text) {
    auto document = parse_json(text);
    if (document.is_discarded()) fail("response contains malformed, duplicate-key, or excessively nested JSON");
    return document;
}

ModelReply normalize_reply(const Json& chat_document, const Json& history) {
    return parse_reply(chat_document, history);
}
}

ProviderApi parse_provider_api(const std::string& value) {
    if (value == "chat-completions") return ProviderApi::chat_completions;
    if (value == "responses") return ProviderApi::responses;
    if (value == "messages") return ProviderApi::messages;
    fail("API must be chat-completions, responses, or messages");
}

std::string provider_api_name(ProviderApi api) {
    switch (api) {
        case ProviderApi::chat_completions: return "chat-completions";
        case ProviderApi::responses: return "responses";
        case ProviderApi::messages: return "messages";
    }
    fail("unknown provider API");
}

std::unique_ptr<ModelProvider> make_openai_compatible_provider(ProviderConfig config) {
    return std::make_unique<OpenAICompatibleProvider>(std::move(config));
}

Json list_local_models(ProviderConfig config, const std::atomic_bool& cancelled) {
    config.allow_insecure_loopback = true;
    config.stream = false;
    config.model = "local-model-catalog";
    config.api = ProviderApi::chat_completions; // OpenAI-compatible /models catalog.
    validate_config(config);
    const auto endpoint = parse_endpoint(config, "/models");
    if (!endpoint.loopback) fail("local model commands require literal 127.0.0.1 or [::1] loopback");
    const auto response = request(endpoint, config, {}, cancelled, {}, true);
    if (response.status < 200 || response.status >= 300)
        fail("HTTP status " + std::to_string(response.status) + " (response body omitted)");
    auto document = protocol::strict_json(response.body);
    // Ollama 0.30.10 encodes an empty OpenAI model list as explicit null.
    // Normalize only that list envelope; missing/malformed catalogs still fail.
    if (document.is_object() && document.contains("object") && document["object"] == "list" &&
        document.contains("data") && document["data"].is_null()) document["data"] = Json::array();
    if (!document.is_object() || !document.contains("data") || !document["data"].is_array() ||
        document["data"].size() > 4096) fail("model catalog requires a data array with at most 4096 entries");
    std::set<std::string> identifiers;
    for (const auto& entry : document["data"]) {
        if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string())
            fail("model catalog entry requires a string id");
        auto id = entry["id"].get<std::string>();
        if (id.empty() || id.size() > 512 || std::any_of(id.begin(), id.end(), is_ascii_control))
            fail("model catalog entry has an invalid id");
        if (!identifiers.insert(std::move(id)).second) fail("model catalog contains a duplicate id");
    }
    Json models = Json::array();
    for (const auto& id : identifiers) models.push_back({{"id", id}});
    return {{"endpoint", endpoint.url}, {"models", std::move(models)}, {"count", identifiers.size()}};
}

Json probe_local_model(ProviderConfig config, const std::atomic_bool& cancelled) {
    config.allow_insecure_loopback = true;
    config.on_text_delta = {}; // Provisional text is irrelevant to this capability check.
    validate_config(config);
    const auto endpoint = parse_endpoint(config);
    if (!endpoint.loopback) fail("local model commands require literal 127.0.0.1 or [::1] loopback");
    Json result{{"endpoint", endpoint.url}, {"model", config.model}, {"api", provider_api_name(config.api)},
                {"stream", config.stream}, {"status", "unsupported"},
                {"capabilities", {{"inference", false}, {"tool_call", false}, {"tool_result", false},
                                   {"streaming", config.stream ? Json(false) : Json(nullptr)}}}};
    constexpr const char* token = "codestudio-probe";
    constexpr const char* acknowledgment = "codestudio-probe-ack";
    const Json schema{{"type", "object"}, {"properties", {{"token", {{"type", "string"}, {"enum", Json::array({token})}}}}},
                      {"required", Json::array({"token"})}, {"additionalProperties", false}};
    const Json definitions = Json::array({{{"type", "function"}, {"function", {
        {"name", "connection_probe"}, {"description", "Harmless connection test. Returns an acknowledgment to repeat."},
        {"parameters", schema}}}}});
    Json history = Json::array({{{"role", "user"}, {"content",
        "This is a harmless API capability test. Call connection_probe exactly once with token codestudio-probe. "
        "After receiving its result, return only the acknowledgment property, without additional text or tool calls."}}});
    try {
        auto provider = make_openai_compatible_provider(config);
        auto first = provider->complete(history, definitions, cancelled);
        result["capabilities"]["inference"] = true;
        if (config.stream) result["capabilities"]["streaming"] = true;
        if (first.calls.size() != 1 || first.calls.front().name != "connection_probe" ||
            first.calls.front().arguments != Json{{"token", token}}) {
            result["reason"] = "model_did_not_return_the_requested_tool_call";
            return result;
        }
        result["capabilities"]["tool_call"] = true;
        history.push_back(std::move(first.message));
        // A synthetic receipt exercises protocol continuation only. No tool is dispatched.
        history.push_back({{"role", "tool"}, {"tool_call_id", first.calls.front().id},
                           {"content", Json{{"acknowledgment", acknowledgment}}.dump()}});
        auto final = provider->complete(history, definitions, cancelled);
        auto text = std::string_view(final.text);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
        if (!final.calls.empty() || text != acknowledgment) {
            result["reason"] = "model_did_not_acknowledge_the_synthetic_tool_result";
            return result;
        }
        result["capabilities"]["tool_result"] = true;
        result["status"] = "compatible";
        result["reason"] = "synthetic_tool_exchange_completed";
    } catch (const std::exception&) {
        // Do not reflect arbitrary provider text, credentials or request bodies.
        result["status"] = cancelled.load() ? "cancelled" : "error";
        result["reason"] = cancelled.load() ? "request_cancelled" : "api_exchange_failed";
    }
    return result;
}

std::unique_ptr<ModelProvider> make_demo_provider() { return std::make_unique<DemoProvider>(); }
}
