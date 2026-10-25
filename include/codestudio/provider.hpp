#pragma once

#include "codestudio/types.hpp"
#include <memory>
#include <functional>
#include <string_view>

namespace codestudio {
struct ModelReply {
    std::string text;
    std::vector<ToolCall> calls;
    Json message = Json::object(); // Normalized assistant history with optional protocol continuation data.
    Json usage = Json::object();
};

class ModelProvider {
public:
    virtual ~ModelProvider() = default;
    virtual ModelReply complete(const Json& messages, const Json& tools,
                                const std::atomic_bool& cancelled) = 0;
    virtual std::string name() const = 0;
};

enum class ProviderApi { chat_completions, responses, messages };
ProviderApi parse_provider_api(const std::string& value);
std::string provider_api_name(ProviderApi api);

struct ProviderConfig {
    std::string base_url;
    std::string model;
    std::string api_key;
    std::chrono::milliseconds timeout{60000};
    std::size_t max_response_bytes = 4 * 1024 * 1024;
    bool allow_insecure_loopback = false;
    ProviderApi api = ProviderApi::chat_completions;
    bool stream = true;
    int max_output_tokens = 4096; // Responses / Messages output budget.
    std::function<void(std::string_view)> on_text_delta; // Provisional display only; never dispatch tools here.
};

// HTTPS only, except explicitly enabled literal loopback HTTP for local models/tests.
std::unique_ptr<ModelProvider> make_openai_compatible_provider(ProviderConfig config);
std::unique_ptr<ModelProvider> make_demo_provider();
}
