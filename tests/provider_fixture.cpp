#include "codestudio/provider.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

// A local-only protocol fixture. Configuration is supplied on stdin so test
// credentials are absent from the command line and error diagnostics.
int main() {
    using namespace codestudio;
    try {
        Json input;
        std::cin >> input;
        std::atomic_bool cancelled{input.value("cancelled", false)};
        std::jthread canceller;
        if (input.contains("cancel_after_ms")) {
            const auto delay = std::chrono::milliseconds(input["cancel_after_ms"].get<int>());
            canceller = std::jthread([&, delay](std::stop_token stop) {
                const auto until = std::chrono::steady_clock::now() + delay;
                while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (!stop.stop_requested()) cancelled.store(true);
            });
        }
        std::unique_ptr<ModelProvider> provider;
        if (input.value("demo", false)) provider = make_demo_provider();
        else {
            ProviderConfig config;
            config.base_url = input.at("base_url").get<std::string>();
            config.model = input.value("model", "fixture-model");
            config.api_key = input.value("api_key", "");
            config.allow_insecure_loopback = input.value("allow_insecure_loopback", false);
            config.timeout = std::chrono::milliseconds(input.value("timeout_ms", 2000));
            config.max_response_bytes = input.value("max_response_bytes", std::size_t{4 * 1024 * 1024});
            config.api = parse_provider_api(input.value("api", "chat-completions"));
            config.stream = input.value("stream", false);
            config.max_output_tokens = input.value("max_output_tokens", 4096);
            if (input.value("emit_deltas", false)) config.on_text_delta = [](std::string_view text) {
                std::cerr << Json{{"delta", text}}.dump() << '\n' << std::flush;
            };
            provider = make_openai_compatible_provider(std::move(config));
        }
        Json messages = input.value("messages", Json::array({{{"role", "user"}, {"content", "Run fixture task."}}}));
        Json tools = input.value("tools", Json::array({
            {{"type", "function"}, {"function", {{"name", "write_file"}, {"parameters", {{"type", "object"}}}}}},
            {{"type", "function"}, {"function", {{"name", "read_file"}, {"parameters", {{"type", "object"}}}}}}
        }));
        std::map<std::string, std::string> files;
        Json replies = Json::array();
        const auto rounds = input.value("rounds", 1);
        for (int round = 0; round < rounds; ++round) {
            auto reply = provider->complete(messages, tools, cancelled);
            replies.push_back({{"text", reply.text}, {"message", reply.message}, {"usage", reply.usage}});
            messages.push_back(reply.message);
            for (const auto& call : reply.calls) {
                Json output;
                if (call.name == "write_file") {
                    const auto path = call.arguments.at("path").get<std::string>();
                    const auto content = call.arguments.at("content").get<std::string>();
                    files[path] = content;
                    output = {{"bytes", content.size()}};
                } else if (call.name == "read_file") {
                    output = {{"content", files.at(call.arguments.at("path").get<std::string>())}};
                    if (input.value("corrupt_read", false)) output["content"] = "corrupted";
                } else throw std::runtime_error("Fixture received unknown tool");
                messages.push_back({{"role", "tool"}, {"tool_call_id", call.id},
                    {"content", Json{{"ok", true}, {"output", output}, {"error", ""}, {"effect", "none"}}.dump()}});
            }
            if (reply.calls.empty()) break;
        }
        std::cout << Json{{"ok", true}, {"replies", replies}, {"messages", messages}, {"files", files}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cout << codestudio::Json{{"ok", false}, {"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
