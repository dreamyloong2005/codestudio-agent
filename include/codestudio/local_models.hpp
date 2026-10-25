#pragma once
#include "codestudio/provider.hpp"

namespace codestudio {
// Local literal-loopback endpoints only. Never discovers credentials or starts
// servers. Empty api_key is valid. Caller must explicitly choose any key.
Json list_local_models(ProviderConfig config, const std::atomic_bool& cancelled);
// Synthetic harmless function-call exchange; does not execute any proposed tool.
Json probe_local_model(ProviderConfig config, const std::atomic_bool& cancelled);
}
