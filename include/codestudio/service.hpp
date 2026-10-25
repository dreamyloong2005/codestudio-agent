#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace codestudio {
// Parent-owned, newline-delimited JSON-RPC. Startup failures use stderr only.
int serve_stdio(const std::vector<std::string>& argv, const std::atomic_bool& shutdown);
}
