#pragma once
#include "codestudio/provider.hpp"

namespace codestudio::protocol {
// Shared strict parser rejects duplicate keys and excessive nesting.
Json strict_json(std::string_view text);
// Validate normalized text/function calls using the same rules for every API.
ModelReply normalize_reply(const Json& chat_document, const Json& history);

class StreamDecoder {
public:
    virtual ~StreamDecoder() = default;
    virtual void event(std::string_view name, std::string_view data) = 0;
    virtual ModelReply finish() = 0; // Requires a successful protocol terminal event.
};

Json responses_request(const ProviderConfig&, const Json& history, const Json& tools);
ModelReply responses_reply(const Json& document, const Json& history);
std::unique_ptr<StreamDecoder> responses_stream(const ProviderConfig&, const Json& history);

Json messages_request(const ProviderConfig&, const Json& history, const Json& tools);
ModelReply messages_reply(const Json& document, const Json& history);
std::unique_ptr<StreamDecoder> messages_stream(const ProviderConfig&, const Json& history);
}
