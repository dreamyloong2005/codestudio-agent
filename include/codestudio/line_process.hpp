#pragma once
#include "codestudio/types.hpp"
#include <memory>
#include <string_view>

namespace codestudio {
// Bounded persistent child transport. stdout is newline-delimited UTF-8 RPC;
// stderr is drained separately and never interpreted as protocol or printed.
// No implicit shell. Destruction/timeout/cancel kills the owned process tree.
class LineProcess {
public:
    using Deadline = std::chrono::steady_clock::time_point;
    explicit LineProcess(const ProcessRequest& request, const std::atomic_bool* cancelled = nullptr);
    ~LineProcess();
    LineProcess(const LineProcess&) = delete;
    LineProcess& operator=(const LineProcess&) = delete;
    void send(std::string_view line, Deadline deadline, const std::atomic_bool& cancelled);
    std::string receive(Deadline deadline, const std::atomic_bool& cancelled);
    void terminate() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
