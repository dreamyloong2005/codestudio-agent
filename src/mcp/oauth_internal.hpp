#pragma once
#include "codestudio/mcp_oauth.hpp"
#include <map>
#include <memory>
#include <stdexcept>

namespace codestudio::oauth_internal {
using Deadline = std::chrono::steady_clock::time_point;
struct Response {
    unsigned status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};
struct Url { std::string url, origin, path, host; unsigned short port = 443; bool secure = true, loopback = false; };
[[noreturn]] void fail(const char*);
void require(bool, const char*);
void check(Deadline, const std::atomic_bool&);
Url url(const std::string&, bool allow_http_loopback);
Response request(const std::string& url, bool allow_http_loopback, const std::string& body,
                 Deadline, const std::atomic_bool&);
std::string random_secret();
std::string encode(std::string_view);
std::string pkce(std::string_view);
// Holds a cross-process OS lock for one system credential; no secret data.
class CredentialLock {
public:
    CredentialLock(const std::string&, Deadline, const std::atomic_bool&);
    ~CredentialLock();
    CredentialLock(const CredentialLock&) = delete;
    CredentialLock& operator=(const CredentialLock&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Callback {
public:
    Callback();
    ~Callback();
    std::string redirect_uri() const;
    std::string wait(const std::string& state, const std::string& issuer, bool issuer_required,
                     Deadline, const std::atomic_bool&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
