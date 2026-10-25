#include "../src/provider/protocol.hpp"

#include <functional>
#include <stdexcept>
#include <string>

namespace {
using namespace codestudio;
using namespace codestudio::protocol;

void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(std::string("responses: ") + message);
}
void rejected(const std::function<void()>& action, const char* message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    expect(failed, message);
}

Json history() { return Json::array({{{"role", "system"}, {"content", "Be helpful."}}, {{"role", "user"}, {"content", "Read a file."}}}); }
Json message(std::string text = "Hello") {
    return {{"id", "msg_1"}, {"type", "message"}, {"role", "assistant"}, {"status", "completed"},
            {"content", Json::array({{{"type", "output_text"}, {"text", std::move(text)}, {"annotations", Json::array()}}})}};
}
Json call(std::string arguments = R"({"path":"README.md"})") {
    return {{"id", "fc_1"}, {"type", "function_call"}, {"call_id", "call_1"}, {"name", "read_file"},
            {"arguments", std::move(arguments)}, {"status", "completed"}};
}
Json reasoning() {
    return {{"id", "rs_1"}, {"type", "reasoning"}, {"summary", Json::array({{{"type", "summary_text"}, {"text", "private summary"}}})},
            {"encrypted_content", "opaque-encrypted-continuation"}};
}
Json response(Json output) {
    return {{"id", "resp_1"}, {"status", "completed"}, {"output", std::move(output)},
            {"usage", {{"input_tokens", 23}, {"output_tokens", 7}, {"total_tokens", 30}}}};
}
void emit(StreamDecoder& stream, const char* type, Json event) {
    event["type"] = type;
    stream.event(type, event.dump());
}
void start_item(StreamDecoder& stream, Json item, int index = 0) {
    item["status"] = "in_progress";
    if (item["type"] == "message") item["content"] = Json::array();
    if (item["type"] == "function_call") item["arguments"] = "";
    emit(stream, "response.output_item.added", {{"output_index", index}, {"item", std::move(item)}});
}
void end_item(StreamDecoder& stream, Json item, int index = 0) {
    emit(stream, "response.output_item.done", {{"output_index", index}, {"item", std::move(item)}});
}
void arguments(StreamDecoder& stream, const std::string& value, int index = 0) {
    emit(stream, "response.function_call_arguments.delta", {{"output_index", index}, {"item_id", "fc_1"}, {"delta", value}});
    emit(stream, "response.function_call_arguments.done", {{"output_index", index}, {"item_id", "fc_1"}, {"arguments", value}});
}
void complete(StreamDecoder& stream, Json output) {
    emit(stream, "response.completed", {{"response", response(std::move(output))}});
}
} // namespace

void run_responses_tests() {
    ProviderConfig config;
    config.model = "responses-test-model";
    config.api = ProviderApi::responses;
    config.stream = true;
    config.max_output_tokens = 2048;

    // Normalize native output and retain reasoning for stateless follow-up requests.
    {
        const auto native = response(Json::array({reasoning(), message("I will read it."), call()}));
        const auto reply = responses_reply(native, history());
        expect(reply.text == "I will read it." && reply.calls.size() == 1, "text and tool output normalize together");
        expect(reply.calls[0].id == "call_1" && reply.calls[0].arguments["path"] == "README.md", "tool call uses call_id, not item id");
        expect(reply.usage == Json({{"prompt_tokens", 23}, {"completion_tokens", 7}, {"total_tokens", 30}}), "usage maps to common accounting");
        expect(reply.message["_responses_output"] == native["output"], "full native continuation retained");
        auto conversation = history();
        conversation.push_back(reply.message);
        conversation.push_back({{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", R"({"ok":true})"}});
        const Json tools = Json::array({{{"type", "function"}, {"function", {
            {"name", "read_file"}, {"description", "Read UTF-8 text"},
            {"parameters", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}}, {"required", Json::array({"path"})}}}
        }}}});
        const auto payload = responses_request(config, conversation, tools);
        expect(payload["model"] == config.model && payload["stream"] == true && payload["max_output_tokens"] == 2048, "request model and output settings");
        expect(payload["store"] == false && payload["include"] == Json::array({"reasoning.encrypted_content"}), "stateless encrypted reasoning requested");
        expect(payload["input"][0]["role"] == "system" && payload["input"][1]["role"] == "user", "system and user history preserved");
        expect(payload["input"][2] == reasoning() && payload["input"][3] == message("I will read it.") && payload["input"][4] == call(), "native output replayed without duplication");
        expect(payload["input"][5] == Json({{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", R"({"ok":true})"}}), "tool result mapped to native call_id");
        expect(payload["tools"][0]["name"] == "read_file" && payload["tools"][0]["strict"] == false &&
               payload["tools"][0]["parameters"]["required"] == Json::array({"path"}), "tool flattening keeps optional fields optional");
        expect(!payload["input"][2].contains("_responses_output") && !payload.contains("messages"), "private metadata not leaked as wire field");
        auto tampered = conversation;
        tampered[2]["tool_calls"][0]["function"]["arguments"] = R"({"path":"different"})";
        rejected([&] { (void)responses_request(config, tampered, tools); }, "mismatched continuation history rejected");
        conversation.back()["tool_call_id"] = "wrong";
        rejected([&] { (void)responses_request(config, conversation, tools); }, "unknown tool result rejected");
    }
    {
        auto conversation = history();
        auto normalized = responses_reply(response(Json::array({call()})), history()).message;
        normalized.erase("_responses_output");
        conversation.push_back(normalized);
        rejected([&] { (void)responses_request(config, conversation, Json::array()); }, "request cannot omit pending tool output");
        conversation.push_back({{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "file bytes"}});
        const auto payload = responses_request(config, conversation, Json::array());
        expect(payload["input"][2]["type"] == "function_call" && payload["input"][2]["call_id"] == "call_1", "normalized assistant calls can be converted without native data");
        expect(!payload.contains("tools"), "empty tool set omitted");
        conversation.push_back(conversation.back());
        rejected([&] { (void)responses_request(config, conversation, Json::array()); }, "duplicate tool result rejected");
    }
    // Successful streaming text is delivered before finish; all final objects must agree.
    {
        std::string displayed;
        config.on_text_delta = [&](std::string_view text) { displayed += text; };
        auto stream = responses_stream(config, history());
        emit(*stream, "response.created", {{"sequence_number", 0}, {"response", {{"id", "resp_1"}, {"status", "in_progress"}}}});
        start_item(*stream, message());
        emit(*stream, "response.content_part.added", {{"output_index", 0}, {"item_id", "msg_1"}, {"content_index", 0},
            {"part", {{"type", "output_text"}, {"text", ""}, {"annotations", Json::array()}}}});
        emit(*stream, "response.output_text.delta", {{"output_index", 0}, {"item_id", "msg_1"}, {"content_index", 0}, {"delta", "Hel"}});
        expect(displayed == "Hel", "partial text delivered while stream remains open");
        rejected([&] { (void)stream->finish(); }, "partial text stream has no accepted reply");
        emit(*stream, "response.output_text.delta", {{"output_index", 0}, {"item_id", "msg_1"}, {"content_index", 0}, {"delta", "lo"}});
        emit(*stream, "response.output_text.done", {{"output_index", 0}, {"item_id", "msg_1"}, {"content_index", 0}, {"text", "Hello"}});
        emit(*stream, "response.content_part.done", {{"output_index", 0}, {"item_id", "msg_1"}, {"content_index", 0}, {"part", message()["content"][0]}});
        end_item(*stream, message());
        complete(*stream, Json::array({message()}));
        const auto reply = stream->finish();
        expect(reply.text == "Hello" && displayed == "Hello", "completed text emitted once and normalized");
        expect(reply.usage["total_tokens"] == 30, "stream completion usage retained");
    }
    config.on_text_delta = {};
    {
        std::string displayed;
        config.on_text_delta = [&](std::string_view text) { displayed += text; };
        auto stream = responses_stream(config, history());
        start_item(*stream, reasoning());
        emit(*stream, "response.reasoning_summary_text.delta", {{"output_index", 0}, {"item_id", "rs_1"}, {"summary_index", 0}, {"delta", "private summary"}});
        end_item(*stream, reasoning());
        start_item(*stream, call(), 1);
        emit(*stream, "response.function_call_arguments.delta", {{"output_index", 1}, {"item_id", "fc_1"}, {"delta", "{\"path\":"}});
        rejected([&] { (void)stream->finish(); }, "partial executable arguments never produce a reply");
        emit(*stream, "response.function_call_arguments.delta", {{"output_index", 1}, {"item_id", "fc_1"}, {"delta", "\"README.md\"}"}});
        emit(*stream, "response.function_call_arguments.done", {{"output_index", 1}, {"item_id", "fc_1"}, {"arguments", call()["arguments"]}});
        end_item(*stream, call(), 1);
        rejected([&] { (void)stream->finish(); }, "complete tool item still requires response.completed");
        complete(*stream, Json::array({reasoning(), call()}));
        const auto reply = stream->finish();
        expect(reply.calls.size() == 1 && reply.message["_responses_output"][0] == reasoning(), "complete streamed call and encrypted continuation retained");
        expect(displayed.empty(), "reasoning and tool arguments are never user text deltas");
    }
    config.on_text_delta = {};
    {
        auto stream = responses_stream(config, history());
        start_item(*stream, call());
        emit(*stream, "response.function_call_arguments.delta", {{"output_index", 0}, {"item_id", "fc_1"}, {"delta", "{"}});
        rejected([&] { emit(*stream, "response.function_call_arguments.done", {{"output_index", 0}, {"item_id", "fc_1"}, {"arguments", "{}"}}); }, "argument done cannot repair mismatched deltas");
        rejected([&] { (void)stream->finish(); }, "failed stream cannot yield calls");
    }
    {
        auto stream = responses_stream(config, history());
        start_item(*stream, call());
        rejected([&] { arguments(*stream, R"({"path":"a","path":"b"})"); }, "duplicate argument keys rejected before completion");
    }
    {
        auto stream = responses_stream(config, history());
        start_item(*stream, call());
        rejected([&] { arguments(*stream, R"({"path":)"); }, "truncated argument JSON rejected");
    }
    {
        auto stream = responses_stream(config, history());
        start_item(*stream, call());
        rejected([&] { end_item(*stream, call()); }, "missing arguments.done rejected");
    }
    {
        auto stream = responses_stream(config, history());
        start_item(*stream, call());
        arguments(*stream, call()["arguments"].get<std::string>());
        auto changed = call();
        changed["call_id"] = "substituted";
        rejected([&] { end_item(*stream, changed); }, "call identity substitution rejected");
    }
    {
        auto stream = responses_stream(config, history());
        start_item(*stream, call());
        arguments(*stream, call()["arguments"].get<std::string>());
        end_item(*stream, call());
        rejected([&] { complete(*stream, Json::array({call("{}")})); }, "completed response cannot replace streamed arguments");
    }
    {
        auto stream = responses_stream(config, history());
        rejected([&] { complete(*stream, Json::array({call()})); }, "unannounced final tool calls rejected");
    }
    for (const auto* type : {"error", "response.failed", "response.incomplete", "response.cancelled"}) {
        auto stream = responses_stream(config, history());
        start_item(*stream, call());
        arguments(*stream, call()["arguments"].get<std::string>());
        end_item(*stream, call());
        rejected([&] { emit(*stream, type, Json::object()); }, "failure event rejects otherwise complete tool call");
        rejected([&] { (void)stream->finish(); }, "failure event prevents result return");
    }
    {
        auto stream = responses_stream(config, history());
        start_item(*stream, message());
        rejected([&] { emit(*stream, "response.output_text.delta", {{"output_index", 0}, {"item_id", "another-message"}, {"content_index", 0}, {"delta", "bad"}}); }, "item identity checked before text emission");
    }
    {
        auto stream = responses_stream(config, history());
        start_item(*stream, message());
        rejected([&] { emit(*stream, "response.output_text.delta", {{"output_index", 0}, {"item_id", "msg_1"}, {"content_index", 1}, {"delta", "bad"}}); }, "content index gaps rejected");
    }
    {
        auto stream = responses_stream(config, history());
        emit(*stream, "response.created", {{"sequence_number", 4}, {"response", {{"id", "resp_1"}, {"status", "in_progress"}}}});
        rejected([&] { emit(*stream, "response.in_progress", {{"sequence_number", 3}, {"response", {{"id", "resp_1"}, {"status", "in_progress"}}}}); }, "decreasing event sequence rejected");
    }
    {
        auto stream = responses_stream(config, history());
        rejected([&] { stream->event("response.completed", R"({"type":"response.output_item.added"})"); }, "SSE name and JSON type mismatch rejected");
    }
    {
        auto stream = responses_stream(config, history());
        rejected([&] { start_item(*stream, call(), 1); }, "output index gaps rejected");
    }
    {
        auto value = response(Json::array({call()}));
        value["status"] = "incomplete";
        rejected([&] { (void)responses_reply(value, history()); }, "incomplete nonstream response cannot execute calls");
        value["status"] = "completed";
        value["output"][0]["arguments"] = "[]";
        rejected([&] { (void)responses_reply(value, history()); }, "non-object function arguments rejected");
        value["output"][0] = call();
        value["output"].push_back(call());
        rejected([&] { (void)responses_reply(value, history()); }, "duplicate native output IDs rejected");
    }
    {
        auto value = response(Json::array({message()}));
        value["output"][0]["content"][0] = {{"type", "refusal"}, {"refusal", "No"}};
        rejected([&] { (void)responses_reply(value, history()); }, "refusal is not a successful final answer");
        value = response(Json::array({message()}));
        value["usage"]["input_tokens"] = -1;
        rejected([&] { (void)responses_reply(value, history()); }, "negative usage rejected");
    }
}
