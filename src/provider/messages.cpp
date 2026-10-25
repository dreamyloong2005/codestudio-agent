#include "protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace codestudio::protocol {
namespace {
[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("Messages API: " + message);
}

std::string string_field(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
        fail(std::string("missing or invalid ") + key);
    }
    return object[key].get<std::string>();
}

void validate_block(const Json& block, bool complete) {
    const auto type = string_field(block, "type");
    if (type == "text") {
        string_field(block, "text");
    } else if (type == "tool_use") {
        if (string_field(block, "id").empty() || string_field(block, "name").empty() ||
            !block.contains("input") || !block["input"].is_object()) {
            fail("tool_use requires an id, name and object input");
        }
    } else if (type == "thinking") {
        string_field(block, "thinking");
        if (complete && string_field(block, "signature").empty()) fail("thinking block is missing its signature");
        if (block.contains("signature") && !block["signature"].is_string()) fail("invalid thinking signature");
    } else if (type == "redacted_thinking") {
        if (string_field(block, "data").empty()) fail("redacted thinking is missing its data");
    } else {
        fail("unsupported content block type");
    }
}

void merge_usage(Json& destination, const Json& incoming) {
    if (!incoming.is_object()) fail("usage must be an object");
    // message_delta counters are cumulative, not increments. Retain only the
    // documented numeric counters, never arbitrary response metadata.
    for (const auto* key : {"input_tokens", "output_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"}) {
        if (!incoming.contains(key)) continue;
        const auto& value = incoming[key];
        if (!value.is_number_integer() || value < 0) fail("invalid token usage");
        destination[key] = value;
    }
}

std::string text_content(const Json& message) {
    if (!message.contains("content") || message["content"].is_null()) return {};
    return string_field(message, "content");
}

Json assistant_content(const Json& message) {
    const bool has_calls = message.contains("tool_calls") && message["tool_calls"].is_array() && !message["tool_calls"].empty();
    const auto normalized = normalize_reply({{"choices", Json::array({{{"finish_reason", has_calls ? "tool_calls" : "stop"},
                                                                       {"message", message}}})}}, Json::array());
    if (message.contains("_messages_content")) {
        const auto& content = message["_messages_content"];
        if (!content.is_array() || content.empty() || content.size() > 1024) fail("invalid native assistant history");
        std::string native_text;
        std::size_t call_index = 0;
        for (const auto& block : content) {
            validate_block(block, true);
            if (block["type"] == "text") native_text += block["text"].get<std::string>();
            else if (block["type"] == "tool_use") {
                if (call_index >= normalized.calls.size()) fail("native assistant history has extra tool calls");
                const auto& expected = normalized.calls[call_index++];
                if (block["id"] != expected.id || block["name"] != expected.name || block["input"] != expected.arguments) {
                    fail("native tool call conflicts with normalized history");
                }
            }
        }
        if (native_text != normalized.text || call_index != normalized.calls.size()) fail("native assistant content conflicts with normalized history");
        return content;
    }
    Json content = Json::array();
    if (!normalized.text.empty()) content.push_back({{"type", "text"}, {"text", normalized.text}});
    for (const auto& call : normalized.calls) {
        content.push_back({{"type", "tool_use"}, {"id", call.id}, {"name", call.name}, {"input", call.arguments}});
    }
    if (content.empty()) fail("assistant history has no supported content");
    return content;
}

class MessagesStream final : public StreamDecoder {
public:
    MessagesStream(const ProviderConfig& config, const Json& history)
        : on_text_(config.on_text_delta), history_(history) {}

    void event(std::string_view name, std::string_view data) override {
        // Unknown event types are extension points, but their envelopes still
        // have to be valid and cannot bypass the terminal or error boundaries.
        const auto value = strict_json(data);
        if (!value.is_object()) fail("stream event must be a JSON object");
        const auto type = string_field(value, "type");
        if (!name.empty() && name != type) fail("SSE event name does not match its type");
        if (stopped_) fail("received a message event after message_stop");
        if (type == "error") fail("provider returned a streaming error");
        if (type != "message_start" && type != "content_block_start" && type != "content_block_delta" &&
            type != "content_block_stop" && type != "message_delta" && type != "message_stop") {
            if (!started_) fail("stream is missing message_start");
            return;
        }

        if (type == "message_start") {
            if (started_ || !value.contains("message") || !value["message"].is_object()) fail("invalid or duplicate message_start");
            const auto& message = value["message"];
            if (string_field(message, "type") != "message" || string_field(message, "role") != "assistant" ||
                !message.contains("content") || !message["content"].is_array() || !message["content"].empty() ||
                (message.contains("stop_reason") && !message["stop_reason"].is_null())) fail("invalid initial message");
            document_ = message;
            document_["usage"] = Json::object();
            if (message.contains("usage")) merge_usage(document_["usage"], message["usage"]);
            started_ = true;
            return;
        }
        if (!started_) fail("stream is missing message_start");

        if (type == "content_block_start") {
            if (finalizing_ || !all_closed() || index(value) != blocks_.size() || blocks_.size() >= 1024 ||
                !value.contains("content_block")) fail("invalid content_block_start order");
            auto block = value["content_block"];
            validate_block(block, false);
            if (block["type"] == "thinking" && !block.contains("signature")) block["signature"] = "";
            blocks_.push_back({std::move(block), {}, false});
            if (blocks_.back().content["type"] == "text") emit(blocks_.back().content["text"].get<std::string>());
            return;
        }
        if (type == "content_block_delta") {
            if (finalizing_) fail("content delta arrived after message_delta");
            auto& block = open_block(value);
            if (!value.contains("delta")) fail("content delta is missing data");
            const auto& delta = value["delta"];
            const auto delta_type = string_field(delta, "type");
            const auto block_type = string_field(block.content, "type");
            if (delta_type == "text_delta" && block_type == "text") {
                auto text = string_field(delta, "text");
                block.content["text"].get_ref<std::string&>() += text;
                emit(text);
            } else if (delta_type == "input_json_delta" && block_type == "tool_use") {
                if (!block.content["input"].empty()) fail("ambiguous tool input and JSON deltas");
                block.arguments += string_field(delta, "partial_json");
            } else if (delta_type == "thinking_delta" && block_type == "thinking") {
                block.content["thinking"].get_ref<std::string&>() += string_field(delta, "thinking");
            } else if (delta_type == "signature_delta" && block_type == "thinking") {
                block.content["signature"].get_ref<std::string&>() += string_field(delta, "signature");
            } else if (delta_type == "citations_delta" && block_type == "text") {
                if (!delta.contains("citation") || !delta["citation"].is_object()) fail("invalid citation delta");
                if (!block.content.contains("citations")) block.content["citations"] = Json::array();
                if (!block.content["citations"].is_array()) fail("invalid text citations");
                block.content["citations"].push_back(delta["citation"]);
            } else {
                fail("unsupported or mismatched content delta");
            }
            return;
        }
        if (type == "content_block_stop") {
            if (finalizing_) fail("content stop arrived after message_delta");
            auto& block = open_block(value);
            if (block.content["type"] == "tool_use" && !block.arguments.empty()) {
                auto arguments = strict_json(block.arguments);
                if (!arguments.is_object()) fail("tool input is not a complete JSON object");
                block.content["input"] = std::move(arguments);
            }
            validate_block(block.content, true);
            block.closed = true;
            return;
        }
        if (type == "message_delta") {
            if (!all_closed() || !value.contains("delta") || !value["delta"].is_object()) fail("invalid message_delta lifecycle");
            finalizing_ = true;
            const auto& delta = value["delta"];
            if (delta.contains("stop_reason") && !delta["stop_reason"].is_null()) {
                const auto reason = string_field(delta, "stop_reason");
                if (document_.contains("stop_reason") && !document_["stop_reason"].is_null() && document_["stop_reason"] != reason) {
                    fail("conflicting stop reasons");
                }
                document_["stop_reason"] = reason;
            }
            if (delta.contains("stop_sequence")) document_["stop_sequence"] = delta["stop_sequence"];
            if (value.contains("usage")) merge_usage(document_["usage"], value["usage"]);
            return;
        }
        if (!finalizing_ || !all_closed()) fail("message_stop arrived before content completion");
        stopped_ = true;
    }

    ModelReply finish() override {
        if (!started_ || !stopped_) fail("stream ended without message_stop");
        document_["content"] = Json::array();
        for (const auto& block : blocks_) document_["content"].push_back(block.content);
        return messages_reply(document_, history_);
    }

private:
    struct Block { Json content; std::string arguments; bool closed; };
    std::function<void(std::string_view)> on_text_;
    Json history_;
    Json document_;
    std::vector<Block> blocks_;
    bool started_ = false;
    bool finalizing_ = false;
    bool stopped_ = false;

    std::size_t index(const Json& event) const {
        if (!event.contains("index") || !event["index"].is_number_integer() || event["index"] < 0 || event["index"] > 1024) {
            fail("invalid content block index");
        }
        return event["index"].get<std::size_t>();
    }
    Block& open_block(const Json& event) {
        const auto position = index(event);
        if (position >= blocks_.size() || blocks_[position].closed) fail("content event has no open block");
        return blocks_[position];
    }
    bool all_closed() const {
        return std::all_of(blocks_.begin(), blocks_.end(), [](const Block& block) { return block.closed; });
    }
    void emit(const std::string& text) const { if (on_text_ && !text.empty()) on_text_(text); }
};
}

Json messages_request(const ProviderConfig& config, const Json& history, const Json& tools) {
    if (!history.is_array() || !tools.is_array() || config.max_output_tokens <= 0) fail("invalid request configuration");
    Json messages = Json::array();
    Json system = Json::array();
    std::set<std::string> pending_calls;
    std::set<std::string> seen_calls;
    bool previous_was_tool = false;
    for (const auto& message : history) {
        const auto role = string_field(message, "role");
        if (role != "tool" && !pending_calls.empty()) fail("assistant tool calls are missing their results");
        if (role == "system" || role == "developer") {
            const auto text = text_content(message);
            if (!text.empty()) system.push_back({{"type", "text"}, {"text", text}});
        } else if (role == "tool") {
            const auto id = string_field(message, "tool_call_id");
            if (pending_calls.erase(id) != 1) fail("tool result does not match a pending call or is duplicated");
            Json result = {{"type", "tool_result"}, {"tool_use_id", id},
                           {"content", text_content(message)}};
            if (previous_was_tool) messages.back()["content"].push_back(std::move(result));
            else messages.push_back({{"role", "user"}, {"content", Json::array({std::move(result)})}});
        } else if (role == "assistant") {
            auto content = assistant_content(message);
            for (const auto& block : content) {
                if (block["type"] != "tool_use") continue;
                const auto id = block["id"].get<std::string>();
                if (!seen_calls.insert(id).second) fail("tool call IDs must be unique throughout history");
                pending_calls.insert(id);
            }
            messages.push_back({{"role", "assistant"}, {"content", std::move(content)}});
        } else if (role == "user") {
            messages.push_back({{"role", "user"}, {"content", text_content(message)}});
        } else fail("unsupported history role");
        previous_was_tool = role == "tool";
    }
    if (!pending_calls.empty()) fail("assistant tool calls are missing their results");
    Json request = {{"model", config.model}, {"messages", std::move(messages)},
                    {"max_tokens", config.max_output_tokens}, {"stream", config.stream}};
    if (!system.empty()) request["system"] = std::move(system);
    if (!tools.empty()) {
        request["tools"] = Json::array();
        for (const auto& tool : tools) {
            if (string_field(tool, "type") != "function" || !tool.contains("function") || !tool["function"].is_object()) {
                fail("only function tools are supported");
            }
            const auto& function = tool["function"];
            if (!function.contains("parameters") || !function["parameters"].is_object()) fail("tool parameters must be a schema object");
            Json native = {{"name", string_field(function, "name")}, {"input_schema", function["parameters"]}};
            if (function.contains("description")) native["description"] = string_field(function, "description");
            request["tools"].push_back(std::move(native));
        }
    }
    return request;
}

ModelReply messages_reply(const Json& document, const Json& history) {
    if (string_field(document, "type") == "error") fail("provider returned an error");
    if (document["type"] != "message" || string_field(document, "role") != "assistant") fail("response must be an assistant message");
    const auto stop = string_field(document, "stop_reason");
    if (stop == "max_tokens") fail("model output was truncated; increase max_output_tokens before retrying");
    if (stop != "end_turn" && stop != "stop_sequence" && stop != "tool_use") fail("unsupported or incomplete stop_reason");
    if (!document.contains("content") || !document["content"].is_array() || document["content"].size() > 1024) fail("invalid message content");
    std::string text;
    Json calls = Json::array();
    for (const auto& block : document["content"]) {
        validate_block(block, true);
        if (block["type"] == "text") text += block["text"].get<std::string>();
        else if (block["type"] == "tool_use") calls.push_back({{"id", block["id"]}, {"type", "function"},
            {"function", {{"name", block["name"]}, {"arguments", block["input"].dump()}}}});
    }
    Json message = {{"role", "assistant"}, {"content", text}, {"tool_calls", std::move(calls)}};
    auto reply = normalize_reply({{"choices", Json::array({{{"finish_reason", stop == "tool_use" ? "tool_calls" : "stop"},
                                                           {"message", std::move(message)}}})}}, history);
    reply.message["_messages_content"] = document["content"];
    if (document.contains("usage")) {
        merge_usage(reply.usage, document["usage"]);
        if (reply.usage.contains("input_tokens")) {
            // Anthropic reports uncached input separately from both cache token
            // counters. The normalized prompt count includes all three.
            auto prompt_tokens = reply.usage["input_tokens"].get<std::uint64_t>();
            bool fits = true;
            for (const auto* key : {"cache_creation_input_tokens", "cache_read_input_tokens"}) {
                if (!reply.usage.contains(key)) continue;
                const auto cached = reply.usage[key].get<std::uint64_t>();
                if (cached > std::numeric_limits<std::uint64_t>::max() - prompt_tokens) { fits = false; break; }
                prompt_tokens += cached;
            }
            if (fits) reply.usage["prompt_tokens"] = prompt_tokens;
        }
        if (reply.usage.contains("output_tokens")) reply.usage["completion_tokens"] = reply.usage["output_tokens"];
        if (reply.usage.contains("prompt_tokens") && reply.usage.contains("output_tokens")) {
            const auto input = reply.usage["prompt_tokens"].get<std::uint64_t>();
            const auto output = reply.usage["output_tokens"].get<std::uint64_t>();
            if (input <= std::numeric_limits<std::uint64_t>::max() - output) reply.usage["total_tokens"] = input + output;
        }
    }
    return reply;
}

std::unique_ptr<StreamDecoder> messages_stream(const ProviderConfig& config, const Json& history) {
    return std::make_unique<MessagesStream>(config, history);
}
}
