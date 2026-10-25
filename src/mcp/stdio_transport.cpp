#include "transport.hpp"
#include "codestudio/line_process.hpp"
#include "../provider/protocol.hpp"

namespace codestudio::mcp_transport {
namespace {
class StdioTransport final : public Transport {
public:
    StdioTransport(const ProcessRequest& request,const std::atomic_bool& cancelled) : process_(request,&cancelled) {}
    void exchange(const Json& request, Deadline deadline, const std::atomic_bool& cancelled,
                  const Receiver& receive) override {
        send(request, deadline, cancelled);
        std::size_t bytes = 0;
        for (;;) {
            const auto line = process_.receive(deadline, cancelled);
            bytes += line.size();
            if (bytes > 4 * 1024 * 1024) throw std::runtime_error("MCP response stream exceeds size limit.");
            auto action = receive(protocol::strict_json(line));
            if (action.reply) send(*action.reply, deadline, cancelled);
            if (action.complete) return;
        }
    }
    void send(const Json& message, Deadline deadline, const std::atomic_bool& cancelled) override {
        process_.send(message.dump(), deadline, cancelled);
    }
    void negotiated(const std::string&) override {}
    void close() noexcept override { process_.terminate(); }
private:
    LineProcess process_;
};
}
std::unique_ptr<Transport> make_stdio_transport(const ProcessRequest& request,const std::atomic_bool& cancelled) {
    return std::make_unique<StdioTransport>(request,cancelled);
}
}
