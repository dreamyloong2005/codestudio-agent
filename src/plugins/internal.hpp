#pragma once
#include "codestudio/types.hpp"
#include <string>

namespace codestudio::plugin_detail {
constexpr std::size_t max_bundle = 96 * 1024 * 1024;
std::string download(const std::string& url, const std::atomic_bool&, bool allow_http_loopback);
}
