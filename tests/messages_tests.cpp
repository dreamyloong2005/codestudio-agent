#include "../src/provider/protocol.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace codestudio;
using namespace codestudio::protocol;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("messages: " + message);
}

void rejected(const std::function<void()>& operation, const std::string& message) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    expect(failed, message);
}

Json history() { return Json::array({{{"role", "system"}, {"content", "Be precise."}},
                                   {{"role", "user"}, {"content", "Read two files."}}}); }

Json text_block(const std::string& text) { return {{"type", "text"}, {"text", text}}; }
Json tool_block(const std::string& id, const std::string& path) {
    return {{"type", "tool_use"}, {"id", id}, {"name", "read_file"}, {"input", {{"path", path}}}};
}

Json message(const Json& content, const std::string& reason = "end_turn") {
    return {{"type", "message"}, {"id", "msg_123"}, {"role", "assistant"}, {"model", "test"},
            {"content", content}, {"stop_reason", reason}, {"stop_sequence", nullptr},
            {"usage", {{"input_tokens", 7}, {"output_tokens", 4}}}};
}

void event(StreamDecoder& decoder, const std::string& name, Json data) {
    data["type"] = name;
    decoder.event(name, data.dump());
}
void start(StreamDecoder& decoder) {
    auto initial = message(Json::array());
    initial["stop_reason"] = nullptr;
    initial["usage"] = {{"input_tokens", 7}, {"output_tokens", 1}, {"cache_read_input_tokens", 3}};
    event(decoder, "message_start", {{"message", initial}});
}
void block_start(StreamDecoder& decoder, int index, const Json& block) {
    event(decoder, "content_block_start", {{"index", index}, {"content_block", block}});
}
void block_delta(StreamDecoder& decoder, int index, const Json& delta) {
    event(decoder, "content_block_delta", {{"index", index}, {"delta", delta}});
}
void block_stop(StreamDecoder& decoder, int index) {
    event(decoder, "content_block_stop", {{"index", index}});
}
void terminal(StreamDecoder& decoder, const std::string& reason = "end_turn") {
    event(decoder, "message_delta", {{"delta", {{"stop_reason", reason}, {"stop_sequence", nullptr}}},
                                    {"usage", {{"output_tokens", 9}}}});
    event(decoder, "message_stop", Json::object());
}
void text_stream(StreamDecoder& decoder) {
    start(decoder);
    block_start(decoder, 0, text_block(""));
    block_delta(decoder, 0, {{"type", "text_delta"}, {"text", "Hello"}});
    block_delta(decoder, 0, {{"type", "text_delta"}, {"text", " world"}});
    block_stop(decoder, 0);
}
}

void run_messages_tests() {
    ProviderConfig config;
    config.model = "messages-fixture";
    config.api = ProviderApi::messages;
    config.max_output_tokens = 1024;
    Json tools = Json::array({{{"type", "function"}, {"function", {{"name", "read_file"},
        {"description", "Read a UTF-8 file"}, {"parameters", {{"type", "object"},
        {"properties", {{"path", {{"type", "string"}}}}}, {"required", Json::array({"path"})}}}}}}});

    const auto initial_request = messages_request(config, history(), tools);
    expect(initial_request["model"] == config.model && initial_request["max_tokens"] == 1024 && initial_request["stream"] == true,
           "request carries model, required token budget and stream mode");
    expect(initial_request["system"][0]["text"] == "Be precise." && initial_request["messages"].size() == 1,
           "system is separated from conversational messages");
    expect(initial_request["tools"][0]["input_schema"] == tools[0]["function"]["parameters"] &&
           initial_request["tools"][0]["name"] == "read_file", "function schema is translated");

    const Json thinking = {{"type", "thinking"}, {"thinking", "private analysis"}, {"signature", "opaque-signature"}};
    const Json redacted = {{"type", "redacted_thinking"}, {"data", "opaque-redacted-data"}};
    const auto native = Json::array({thinking, redacted, text_block("Checking."), tool_block("call_1", "a.txt"), tool_block("call_2", "b.txt")});
    auto reply = messages_reply(message(native, "tool_use"), history());
    expect(reply.text == "Checking." && reply.calls.size() == 2 && reply.calls[1].arguments["path"] == "b.txt",
           "tool calls normalize without exposing thinking");
    expect(reply.message["_messages_content"] == native, "native signed blocks survive normalization unchanged");
    auto continuation = history();
    continuation.push_back(reply.message);
    continuation.push_back({{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "first"}});
    continuation.push_back({{"role", "tool"}, {"tool_call_id", "call_2"}, {"content", "second"}});
    const auto request = messages_request(config, continuation, tools);
    expect(request["messages"][1]["content"] == native, "signed assistant content is sent back exactly");
    expect(request["messages"][2]["role"] == "user" && request["messages"][2]["content"].size() == 2 &&
           request["messages"][2]["content"][1]["tool_use_id"] == "call_2", "parallel tool results coalesce into one user message");
    expect(request["messages"][2]["content"][0]["content"] == "first", "tool result body survives translation");
    continuation[2].erase("_messages_content");
    const auto reconstructed = messages_request(config, continuation, tools);
    expect(reconstructed["messages"][1]["content"][1] == tool_block("call_1", "a.txt"),
           "ordinary normalized Chat history can be converted to Messages content");
    const auto with_native_history = [&] {
        auto result = continuation;
        result[2] = reply.message;
        return result;
    };
    rejected([&] { auto changed = with_native_history(); changed[2]["_messages_content"][2]["text"] = "Different";
        messages_request(config, changed, tools); }, "native text cannot contradict normalized history");
    for (const auto* field : {"id", "name", "input"}) {
        rejected([&] { auto changed = with_native_history();
            changed[2]["_messages_content"][3][field] = std::string(field) == "input" ? Json{{"path", "other"}} : Json("different");
            messages_request(config, changed, tools); }, std::string("native tool field must match normalized history: ") + field);
    }
    rejected([&] { auto changed = with_native_history(); changed[2]["_messages_content"].erase(4);
        messages_request(config, changed, tools); }, "missing native call cannot suppress a normalized call");
    rejected([&] { auto changed = with_native_history(); changed[2]["_messages_content"].push_back(tool_block("extra", "x"));
        messages_request(config, changed, tools); }, "extra native call cannot enter continuation");
    rejected([&] { auto changed = continuation; changed.erase(4);
        messages_request(config, changed, tools); }, "every pending tool call requires a result");
    rejected([&] { auto changed = continuation; changed[4]["tool_call_id"] = "call_1";
        messages_request(config, changed, tools); }, "duplicate tool results are rejected");
    rejected([&] { auto changed = continuation; changed[4]["tool_call_id"] = "foreign";
        messages_request(config, changed, tools); }, "foreign tool result IDs are rejected");
    rejected([&] { auto changed = history(); changed.push_back(continuation[3]);
        messages_request(config, changed, tools); }, "orphan tool results are rejected");
    rejected([&] { auto changed = continuation; changed.erase(4); changed.push_back({{"role", "user"}, {"content", "Next"}});
        messages_request(config, changed, tools); }, "new messages cannot interrupt pending tool results");
    rejected([&] { auto changed = continuation; changed.push_back(continuation[2]); changed.push_back(continuation[3]); changed.push_back(continuation[4]);
        messages_request(config, changed, tools); }, "resolved tool call IDs cannot be reused in later turns");
    auto reordered_results = continuation;
    std::swap(reordered_results[3], reordered_results[4]);
    expect(messages_request(config, reordered_results, tools)["messages"].back()["content"][0]["tool_use_id"] == "call_2",
           "parallel tool results may arrive in either order");

    std::string displayed;
    config.on_text_delta = [&](std::string_view text) { displayed += text; };
    auto decoder = messages_stream(config, history());
    text_stream(*decoder);
    decoder->event("ping", "{\"type\":\"ping\"}");
    decoder->event("future_event", "{\"type\":\"future_event\",\"metadata\":true}");
    event(*decoder, "message_delta", {{"delta", Json::object()}, {"usage", {{"output_tokens", 6}}}});
    terminal(*decoder);
    auto streamed = decoder->finish();
    expect(displayed == "Hello world" && streamed.text == displayed, "text deltas are emitted once in order");
    expect(streamed.usage["input_tokens"] == 7 && streamed.usage["output_tokens"] == 9 &&
           streamed.usage["prompt_tokens"] == 10 && streamed.usage["total_tokens"] == 19 && streamed.usage["cache_read_input_tokens"] == 3,
           "stream usage merges cumulative counts instead of summing deltas");

    displayed.clear();
    decoder = messages_stream(config, history());
    start(*decoder);
    block_start(*decoder, 0, {{"type", "thinking"}, {"thinking", ""}, {"signature", ""}});
    block_delta(*decoder, 0, {{"type", "thinking_delta"}, {"thinking", "hidden"}});
    block_delta(*decoder, 0, {{"type", "signature_delta"}, {"signature", "signed"}});
    block_stop(*decoder, 0);
    block_start(*decoder, 1, redacted);
    block_stop(*decoder, 1);
    block_start(*decoder, 2, {{"type", "tool_use"}, {"id", "call_3"}, {"name", "read_file"}, {"input", Json::object()}});
    block_delta(*decoder, 2, {{"type", "input_json_delta"}, {"partial_json", ""}});
    block_delta(*decoder, 2, {{"type", "input_json_delta"}, {"partial_json", "{\"path\":"}});
    block_delta(*decoder, 2, {{"type", "input_json_delta"}, {"partial_json", "\"file.txt\"}"}});
    block_stop(*decoder, 2);
    terminal(*decoder, "tool_use");
    streamed = decoder->finish();
    expect(displayed.empty() && streamed.text.empty() && streamed.calls.size() == 1 && streamed.calls[0].arguments["path"] == "file.txt",
           "tool JSON is assembled while reasoning and arguments are not displayed");
    expect(streamed.message["_messages_content"][0]["signature"] == "signed" &&
           streamed.message["_messages_content"][1] == redacted, "streaming preserves signed and redacted thinking for continuation");

    for (const auto& reason : {"max_tokens", "pause_turn", "refusal", "unknown"}) {
        rejected([&] { messages_reply(message(Json::array({text_block("partial")}), reason), history()); },
                 std::string("unsupported or incomplete stop reason rejected: ") + reason);
    }
    rejected([&] { messages_reply(message(Json::array({tool_block("call_1", "x")}), "end_turn"), history()); },
             "tool calls cannot hide under a successful final text reason");
    rejected([&] { messages_reply(message(Json::array({text_block("text")}), "tool_use"), history()); },
             "tool_use stop reason requires a tool call");
    rejected([&] { messages_reply(message(Json::array({tool_block("call_1", "x"), tool_block("call_1", "y")}), "tool_use"), history()); },
             "duplicate tool call IDs rejected");
    rejected([&] { messages_reply(message(Json::array({tool_block("call_1", "x")}), "tool_use"), continuation); },
             "tool IDs already present in history rejected");
    rejected([&] { messages_reply(message(Json::array({{{"type", "thinking"}, {"thinking", "unsigned"}}, text_block("text")})), history()); },
             "unsigned thinking cannot enter continuation history");

    rejected([&] { auto stream = messages_stream(config, history()); text_stream(*stream); stream->finish(); },
             "EOF before message_stop is incomplete");
    rejected([&] { auto stream = messages_stream(config, history()); text_stream(*stream); event(*stream, "message_stop", Json::object()); },
             "message_stop requires message_delta with terminal state");
    rejected([&] { auto stream = messages_stream(config, history()); text_stream(*stream);
        event(*stream, "message_delta", {{"delta", Json::object()}}); event(*stream, "message_stop", Json::object()); stream->finish(); },
             "terminal stream without stop reason rejected");
    rejected([&] { auto stream = messages_stream(config, history()); text_stream(*stream); terminal(*stream, "max_tokens"); stream->finish(); },
             "truncated streamed text is not accepted as final");
    rejected([&] { auto stream = messages_stream(config, history()); start(*stream);
        block_start(*stream, 0, {{"type", "tool_use"}, {"id", "call_4"}, {"name", "read_file"}, {"input", Json::object()}});
        block_delta(*stream, 0, {{"type", "input_json_delta"}, {"partial_json", "{\"path\":"}}); block_stop(*stream, 0); },
             "truncated tool arguments rejected at block completion");
    rejected([&] { auto stream = messages_stream(config, history()); start(*stream);
        block_start(*stream, 0, {{"type", "tool_use"}, {"id", "call_4"}, {"name", "read_file"}, {"input", Json::object()}});
        block_delta(*stream, 0, {{"type", "input_json_delta"}, {"partial_json", "{\"path\":\"a\",\"path\":\"b\"}"}}); block_stop(*stream, 0); },
             "duplicate keys in streamed executable arguments rejected");
    rejected([&] { auto stream = messages_stream(config, history()); start(*stream); block_start(*stream, 0, text_block("partial"));
        terminal(*stream); }, "unfinished content block prevents a completed message");
    rejected([&] { auto stream = messages_stream(config, history()); start(*stream); block_start(*stream, 1, text_block("")); },
             "missing content index rejected");
    rejected([&] { auto stream = messages_stream(config, history()); start(*stream); start(*stream); },
             "duplicate message_start rejected");
    rejected([&] { auto stream = messages_stream(config, history()); start(*stream); block_delta(*stream, 0, {{"type", "text_delta"}, {"text", "bad"}}); },
             "delta without an open content block rejected");
    rejected([&] { auto stream = messages_stream(config, history()); text_stream(*stream); terminal(*stream);
        block_start(*stream, 1, text_block("late")); }, "content after successful terminal rejected");
    rejected([&] { auto stream = messages_stream(config, history()); text_stream(*stream);
        event(*stream, "message_delta", {{"delta", {{"stop_reason", "end_turn"}}}, {"usage", {{"output_tokens", -1}}}}); },
             "negative cumulative usage rejected");
    rejected([&] { auto stream = messages_stream(config, history()); text_stream(*stream);
        event(*stream, "error", {{"error", {{"type", "overloaded_error"}, {"message", "Overloaded"}}}}); },
             "stream provider error prevents completion");
    rejected([&] { auto stream = messages_stream(config, history()); stream->event("message_start", "{\"type\":\"message_stop\"}"); },
             "SSE name and JSON type mismatch rejected");
    rejected([&] { auto stream = messages_stream(config, history()); start(*stream);
        stream->event("ping", "{\"type\":\"error\",\"error\":{\"message\":\"failed\"}}"); },
             "extension event names cannot conceal an error envelope");
    rejected([&] { auto stream = messages_stream(config, history()); start(*stream); stream->event("future_event", "not JSON"); },
             "unknown events must still have valid JSON envelopes");
    rejected([&] { auto stream = messages_stream(config, history()); text_stream(*stream); terminal(*stream);
        stream->event("future_event", "{\"type\":\"future_event\"}"); }, "unknown events after message_stop are rejected");
}
