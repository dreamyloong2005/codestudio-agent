#pragma once

#include "codestudio/types.hpp"
#include <functional>
#include <memory>
#include <optional>

namespace codestudio::mcp_transport {

using Deadline = std::chrono::steady_clock::time_point;

struct MessageAction {
    bool complete = false;
    std::optional<Json> reply;
};
using Receiver = std::function<MessageAction(const Json&)>;

// The protocol layer owns JSON-RPC validation and correlation. Transports own
// framing, deadlines and resource lifetime, and must never replay a request.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void exchange(const Json&, Deadline, const std::atomic_bool&, const Receiver&) = 0;
    virtual void send(const Json&, Deadline, const std::atomic_bool&) = 0;
    virtual void negotiated(const std::string&) = 0;
    virtual void close() noexcept = 0;
};

struct HttpOptions {
    std::string url;
    std::string bearer;
    bool allow_http_loopback = false;
};

void validate_http_endpoint(const std::string&, bool);
std::unique_ptr<Transport> make_http_transport(HttpOptions);
std::unique_ptr<Transport> make_stdio_transport(const ProcessRequest&, const std::atomic_bool&);

}
