#include "protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace codestudio::protocol {
namespace {
[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(std::string("Responses API: ") + message);
}

std::string string_field(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string())
        fail("missing or invalid string field");
    return object[key].get<std::string>();
}

std::string identifier(const Json& object, const char* key) {
    auto result = string_field(object, key);
    if (result.empty() || result.size() > 256 || std::any_of(result.begin(), result.end(), [](unsigned char c) {
        return c <= 32 || c >= 127;
    })) fail("invalid item identifier");
    return result;
}

std::size_t index_field(const Json& object, const char* key, std::size_t limit) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_number_integer() ||
        object[key] < 0 || object[key] >= limit) fail("invalid stream index");
    return object[key].get<std::size_t>();
}

void complete_item(const Json& item) {
    if (item.contains("status") && !item["status"].is_null() && item["status"] != "completed")
        fail("output item did not complete");
}

void validate_reasoning(const Json& item) {
    if (item.contains("encrypted_content") && !item["encrypted_content"].is_null() &&
        !item["encrypted_content"].is_string()) fail("invalid encrypted reasoning continuation");
    if (item.contains("summary")) {
        if (!item["summary"].is_array()) fail("invalid reasoning summary");
        for (const auto& summary : item["summary"]) {
            if (string_field(summary, "type") != "summary_text") fail("unsupported reasoning summary");
            (void)string_field(summary, "text");
        }
    }
    if (item.contains("content") && !item["content"].is_null()) {
        if (!item["content"].is_array()) fail("invalid reasoning content");
        for (const auto& content : item["content"]) {
            if (string_field(content, "type") != "reasoning_text") fail("unsupported reasoning content");
            (void)string_field(content, "text");
        }
    }
}

std::string text_part(const Json& part) {
    const auto type = string_field(part, "type");
    if (type == "refusal") fail("model refused the request");
    if (type != "output_text") fail("unsupported response content type");
    if (part.contains("annotations") && !part["annotations"].is_array()) fail("invalid text annotations");
    return string_field(part, "text");
}

Json chat_document(const std::string& text, const Json& calls, Json usage = Json::object()) {
    Json message{{"role", "assistant"}, {"content", text}};
    if (!calls.empty()) message["tool_calls"] = calls;
    return {{"choices", Json::array({{{"finish_reason", calls.empty() ? "stop" : "tool_calls"},
                                      {"message", std::move(message)}}})}, {"usage", std::move(usage)}};
}

Json converted_usage(const Json& document) {
    Json result = Json::object();
    if (!document.contains("usage") || document["usage"].is_null()) return result;
    if (!document["usage"].is_object()) fail("invalid usage accounting");
    const auto& usage = document["usage"];
    for (const auto& [source, destination] : {std::pair{"input_tokens", "prompt_tokens"},
                                            std::pair{"output_tokens", "completion_tokens"},
                                            std::pair{"total_tokens", "total_tokens"}}) {
        if (usage.contains(source)) {
            if (!usage[source].is_number_integer() || usage[source] < 0) fail("invalid token count");
            result[destination] = usage[source];
        }
    }
    return result;
}

void same_calls(const ModelReply& native, const ModelReply& normalized) {
    if (native.text != normalized.text || native.calls.size() != normalized.calls.size())
        fail("native continuation does not match assistant history");
    for (std::size_t i = 0; i < native.calls.size(); ++i) {
        const auto& a = native.calls[i];
        const auto& b = normalized.calls[i];
        if (a.id != b.id || a.name != b.name || a.arguments != b.arguments)
            fail("native continuation does not match assistant tool calls");
    }
}

class ResponsesStream final : public StreamDecoder {
public:
    ResponsesStream(const ProviderConfig& config, Json history) : config_(config), history_(std::move(history)) {}

    void event(std::string_view name, std::string_view data) override {
        try { receive(name, data); }
        catch (...) { failed_ = true; throw; }
    }

    ModelReply finish() override {
        if (failed_ || !completed_ || finished_) fail("stream ended without a valid complete response");
        finished_ = true;
        return responses_reply(final_, history_);
    }

private:
    struct Text {
        std::string value;
        bool delta = false;
        bool added = false;
        bool done = false;
        bool part_done = false;
    };
    struct Item {
        std::string id;
        std::string type;
        std::string call_id;
        std::string name;
        std::string arguments;
        bool argument_delta = false;
        bool arguments_done = false;
        std::map<std::size_t, Text> text;
        bool done = false;
        Json value;
    };

    Item& event_item(const Json& value) {
        const auto index = index_field(value, "output_index", 256);
        if (index >= items_.size()) fail("event references an unannounced output item");
        auto& item = items_[index];
        if (identifier(value, "item_id") != item.id || item.done) fail("output item identity or ordering mismatch");
        return item;
    }

    Text& text_state(Item& item, const Json& value) {
        if (item.type != "message") fail("text event targets a non-message item");
        const auto index = index_field(value, "content_index", 4096);
        if (index > item.text.size()) fail("non-sequential text content index");
        return item.text[index];
    }

    void append(std::string& destination, const std::string& value) const {
        if (destination.size() > config_.max_response_bytes || value.size() > config_.max_response_bytes - destination.size())
            fail("stream field exceeds response size limit");
        destination += value;
    }

    void response_identity(const Json& response) {
        if (!response.is_object()) fail("invalid response lifecycle event");
        const auto id = identifier(response, "id");
        if (response_id_.empty()) response_id_ = id;
        else if (response_id_ != id) fail("response identifier changed during stream");
    }

    void receive(std::string_view name, std::string_view data) {
        if (failed_ || completed_ || finished_) fail("event after terminal response");
        const auto value = strict_json(data);
        const auto type = string_field(value, "type");
        if (!name.empty() && name != "message" && name != type) fail("SSE event name does not match payload type");
        if (value.contains("sequence_number")) {
            if (!value["sequence_number"].is_number_integer() || value["sequence_number"] < 0)
                fail("invalid event sequence number");
            const auto sequence = value["sequence_number"].get<std::uint64_t>();
            if (last_sequence_ && sequence <= *last_sequence_) fail("out-of-order event sequence");
            last_sequence_ = sequence;
        }
        if (type == "error" || type == "response.failed" || type == "response.incomplete" || type == "response.cancelled")
            fail("response failed or was incomplete");
        if (type == "response.created" || type == "response.in_progress") {
            if (!value.contains("response")) fail("missing response lifecycle payload");
            if (type == "response.created") {
                if (created_ || !items_.empty()) fail("duplicate or late response.created event");
                created_ = true;
            }
            response_identity(value["response"]);
            if (value["response"].contains("status") && value["response"]["status"] != "in_progress" &&
                value["response"]["status"] != "queued") fail("invalid nonterminal response status");
            return;
        }
        if (type == "response.output_item.added") {
            if (index_field(value, "output_index", 256) != items_.size() || !value.contains("item"))
                fail("duplicate or non-sequential output item");
            const auto& source = value["item"];
            Item item;
            item.id = identifier(source, "id");
            if (!item_ids_.insert(item.id).second) fail("duplicate output item identifier");
            item.type = string_field(source, "type");
            if (item.type == "function_call") {
                item.call_id = identifier(source, "call_id");
                item.name = string_field(source, "name");
                if (string_field(source, "arguments") != "") fail("function call was not introduced with empty arguments");
            } else if (item.type == "message") {
                if (string_field(source, "role") != "assistant" || !source.contains("content") ||
                    !source["content"].is_array() || !source["content"].empty()) fail("invalid initial assistant message");
            } else if (item.type != "reasoning") fail("unsupported streamed output item");
            items_.push_back(std::move(item));
            return;
        }
        if (type == "response.function_call_arguments.delta" || type == "response.function_call_arguments.done") {
            auto& item = event_item(value);
            if (item.type != "function_call" || item.arguments_done) fail("function argument event ordering mismatch");
            if (value.contains("name") && string_field(value, "name") != item.name) fail("function name changed during stream");
            if (type.ends_with(".delta")) {
                item.argument_delta = true;
                append(item.arguments, string_field(value, "delta"));
            } else {
                const auto arguments = string_field(value, "arguments");
                if (item.argument_delta && item.arguments != arguments) fail("completed function arguments differ from deltas");
                if (!item.argument_delta) append(item.arguments, arguments);
                if (!strict_json(item.arguments).is_object()) fail("function arguments must be a complete JSON object");
                item.arguments_done = true;
            }
            return;
        }
        if (type == "response.output_text.delta" || type == "response.output_text.done") {
            auto& item = event_item(value);
            auto& text = text_state(item, value);
            if (text.done || text.part_done) fail("text event after completed content");
            if (type.ends_with(".delta")) {
                const auto delta = string_field(value, "delta");
                text.delta = true;
                append(text.value, delta);
                if (config_.on_text_delta && !delta.empty()) config_.on_text_delta(delta);
            } else {
                const auto complete = string_field(value, "text");
                if (text.delta && text.value != complete) fail("completed text differs from deltas");
                if (!text.delta) {
                    append(text.value, complete);
                    if (config_.on_text_delta && !complete.empty()) config_.on_text_delta(complete);
                }
                text.done = true;
            }
            return;
        }
        if (type == "response.content_part.added" || type == "response.content_part.done") {
            auto& item = event_item(value);
            auto& text = text_state(item, value);
            if (!value.contains("part")) fail("missing content part");
            const auto content = text_part(value["part"]);
            if (type.ends_with(".added")) {
                if (text.added || text.delta || text.done || text.part_done || !content.empty())
                    fail("duplicate or late content part start");
                text.added = true;
            } else {
                if (text.part_done || text.value != content) fail("completed content part differs from streamed text");
                text.part_done = true;
            }
            return;
        }
        if (type == "response.output_item.done") {
            const auto index = index_field(value, "output_index", 256);
            if (index >= items_.size() || !value.contains("item")) fail("unannounced completed output item");
            auto& item = items_[index];
            const auto& source = value["item"];
            if (item.done || identifier(source, "id") != item.id || string_field(source, "type") != item.type)
                fail("completed output item identity mismatch");
            complete_item(source);
            if (item.type == "function_call") {
                if (!item.arguments_done || identifier(source, "call_id") != item.call_id ||
                    string_field(source, "name") != item.name || string_field(source, "arguments") != item.arguments)
                    fail("completed function call differs from streamed call");
            } else if (item.type == "message") {
                if (string_field(source, "role") != "assistant" || !source.contains("content") ||
                    !source["content"].is_array() || source["content"].size() != item.text.size())
                    fail("completed message differs from streamed content");
                for (std::size_t i = 0; i < item.text.size(); ++i) {
                    if (text_part(source["content"][i]) != item.text.at(i).value)
                        fail("completed message text differs from deltas");
                }
            } else validate_reasoning(source);
            item.done = true;
            item.value = source;
            return;
        }
        if (type == "response.completed") {
            if (!value.contains("response")) fail("missing completed response");
            const auto& response = value["response"];
            response_identity(response);
            if (!response.contains("output") || !response["output"].is_array() || response["output"].size() != items_.size())
                fail("completed response has unannounced output items");
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (!items_[i].done || response["output"][i] != items_[i].value)
                    fail("completed response differs from streamed output");
            }
            (void)responses_reply(response, history_);
            final_ = response;
            completed_ = true;
            return;
        }
        if (type.starts_with("response.refusal.")) fail("model refused the request");
        // Reasoning is retained only in the final native output, never displayed.
        if (type == "response.reasoning_summary_part.added" || type == "response.reasoning_summary_part.done" ||
            type == "response.reasoning_summary_text.delta" || type == "response.reasoning_summary_text.done" ||
            type == "response.reasoning_text.delta" || type == "response.reasoning_text.done") {
            if (event_item(value).type != "reasoning") fail("reasoning event targets a non-reasoning item");
            return;
        }
        if (type == "response.output_text.annotation.added") {
            auto& item = event_item(value);
            if (item.type != "message" || index_field(value, "content_index", 4096) >= item.text.size())
                fail("annotation references unannounced text");
            return;
        }
        fail("unsupported streaming event type");
    }

    ProviderConfig config_;
    Json history_;
    std::vector<Item> items_;
    std::set<std::string> item_ids_;
    std::optional<std::uint64_t> last_sequence_;
    std::string response_id_;
    Json final_;
    bool created_ = false;
    bool completed_ = false;
    bool finished_ = false;
    bool failed_ = false;
};
} // namespace

ModelReply responses_reply(const Json& document, const Json& history) {
    if (!document.is_object() || !document.contains("status") || document["status"] != "completed")
        fail("response failed or was incomplete");
    if (document.contains("error") && !document["error"].is_null()) fail("response contains an error");
    if (document.contains("incomplete_details") && !document["incomplete_details"].is_null())
        fail("response contains incomplete output");
    if (!document.contains("output") || !document["output"].is_array() || document["output"].size() > 256)
        fail("missing or invalid response output");
    std::string text;
    Json calls = Json::array();
    std::set<std::string> ids;
    for (const auto& item : document["output"]) {
        if (!ids.insert(identifier(item, "id")).second) fail("duplicate output item identifier");
        complete_item(item);
        const auto type = string_field(item, "type");
        if (type == "message") {
            if (string_field(item, "role") != "assistant" || !item.contains("content") || !item["content"].is_array())
                fail("invalid assistant output message");
            for (const auto& content : item["content"]) text += text_part(content);
        } else if (type == "function_call") {
            calls.push_back({{"id", identifier(item, "call_id")}, {"type", "function"},
                {"function", {{"name", string_field(item, "name")}, {"arguments", string_field(item, "arguments")}}}});
        } else if (type == "reasoning") validate_reasoning(item);
        else fail("unsupported response output item");
    }
    auto result = normalize_reply(chat_document(text, calls, converted_usage(document)), history);
    result.message["_responses_output"] = document["output"];
    return result;
}

Json responses_request(const ProviderConfig& config, const Json& history, const Json& tools) {
    if (!history.is_array() || history.empty() || !tools.is_array()) fail("history and tools must be arrays");
    Json input = Json::array();
    Json preceding = Json::array();
    std::set<std::string> pending;
    for (const auto& message : history) {
        const auto role = string_field(message, "role");
        if (role == "assistant") {
            if (!pending.empty()) fail("assistant message precedes tool results");
            const auto calls = message.contains("tool_calls") && !message["tool_calls"].is_null() ? message["tool_calls"] : Json::array();
            const auto text = message.contains("content") && !message["content"].is_null() ? string_field(message, "content") : std::string{};
            auto normalized = normalize_reply(chat_document(text, calls), preceding);
            if (message.contains("_responses_output")) {
                auto native = responses_reply({{"status", "completed"}, {"output", message["_responses_output"]}}, preceding);
                same_calls(native, normalized);
                for (const auto& item : native.message["_responses_output"]) input.push_back(item);
            } else {
                if (!text.empty()) input.push_back({{"role", "assistant"}, {"content", text}});
                for (const auto& call : normalized.calls) input.push_back({{"type", "function_call"},
                    {"call_id", call.id}, {"name", call.name}, {"arguments", call.arguments.dump()}});
            }
            for (const auto& call : normalized.calls) pending.insert(call.id);
        } else if (role == "tool") {
            const auto id = identifier(message, "tool_call_id");
            if (!pending.erase(id)) fail("tool result references an unknown or completed call");
            input.push_back({{"type", "function_call_output"}, {"call_id", id}, {"output", string_field(message, "content")}});
        } else if (role == "user" || role == "system" || role == "developer") {
            if (!pending.empty()) fail("input message precedes tool results");
            input.push_back({{"role", role}, {"content", string_field(message, "content")}});
        } else fail("unsupported history role");
        preceding.push_back(message);
    }
    if (!pending.empty()) fail("missing tool results in request history");
    Json result{{"model", config.model}, {"input", std::move(input)}, {"stream", config.stream},
                {"store", false}, {"include", Json::array({"reasoning.encrypted_content"})},
                {"max_output_tokens", config.max_output_tokens}};
    if (!tools.empty()) {
        Json flattened = Json::array();
        for (const auto& tool : tools) {
            if (string_field(tool, "type") != "function" || !tool.contains("function") || !tool["function"].is_object())
                fail("only function tools are supported");
            const auto& function = tool["function"];
            if (!function.contains("parameters") || !function["parameters"].is_object()) fail("tool parameters must be an object");
            Json flat{{"type", "function"}, {"name", string_field(function, "name")},
                      {"parameters", function["parameters"]}, {"strict", false}};
            if (function.contains("description")) flat["description"] = string_field(function, "description");
            flattened.push_back(std::move(flat));
        }
        result["tools"] = std::move(flattened);
        result["tool_choice"] = "auto";
        result["parallel_tool_calls"] = false;
    }
    return result;
}

std::unique_ptr<StreamDecoder> responses_stream(const ProviderConfig& config, const Json& history) {
    return std::make_unique<ResponsesStream>(config, history);
}
} // namespace codestudio::protocol
