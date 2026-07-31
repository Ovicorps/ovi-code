#include "ovicode/providers.hpp"

#include <cctype>
#include <random>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "ovicode/http.hpp"

namespace ovicode {

using json = nlohmann::json;

namespace {

constexpr const char* kOpenRouterUrl =
    "https://openrouter.ai/api/v1/chat/completions";

std::string normalizeApiKey(const std::string& value) {
    auto first = value.begin();
    while (first != value.end() &&
           std::isspace(static_cast<unsigned char>(*first))) {
        ++first;
    }
    auto last = value.end();
    while (last != first &&
           std::isspace(static_cast<unsigned char>(*(last - 1)))) {
        --last;
    }

    std::string key(first, last);
    constexpr const char* kBearerPrefix = "Bearer ";
    if (key.starts_with(kBearerPrefix)) {
        key.erase(0, std::char_traits<char>::length(kBearerPrefix));
    }
    return key;
}

std::string genId() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream ss;
    ss << "call_" << std::hex << rng();
    return ss.str();
}

std::string extractApiError(const std::string& body) {
    if (body.empty()) return "<empty response body>";
    try {
        const auto response = json::parse(body);
        if (response.is_object() && response.contains("error")) {
            const auto& error = response["error"];
            if (error.is_object() && error.contains("message") &&
                error["message"].is_string()) {
                return error["message"].get<std::string>();
            }
            if (error.is_string()) return error.get<std::string>();
        }
        if (response.is_object() && response.contains("message") &&
            response["message"].is_string()) {
            return response["message"].get<std::string>();
        }
    } catch (...) {
    }
    return body;
}

json toolsAsOpenAI(const std::vector<ToolDefinition>& tools) {
    json result = json::array();
    for (const auto& tool : tools) {
        result.push_back({
            {"type", "function"},
            {"function", {
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", tool.input_schema},
            }},
        });
    }
    return result;
}

json messagesAsOpenAI(const ChatRequest& req) {
    json messages = json::array();
    if (!req.system.empty()) {
        messages.push_back({{"role", "system"}, {"content", req.system}});
    }
    for (const auto& message : req.messages) {
        switch (message.role) {
            case Message::Role::User:
                messages.push_back(
                    {{"role", "user"}, {"content", message.text}});
                break;
            case Message::Role::Assistant: {
                json item = {{"role", "assistant"}};
                if (!message.text.empty()) item["content"] = message.text;
                else if (message.tool_calls.empty()) item["content"] = "";
                else item["content"] = nullptr;

                if (!message.tool_calls.empty()) {
                    json calls = json::array();
                    for (const auto& call : message.tool_calls) {
                        calls.push_back({
                            {"id", call.id.empty() ? genId() : call.id},
                            {"type", "function"},
                            {"function", {
                                {"name", call.name},
                                {"arguments", call.input.is_null()
                                                  ? std::string("{}")
                                                  : call.input.dump()},
                            }},
                        });
                    }
                    item["tool_calls"] = std::move(calls);
                }
                messages.push_back(std::move(item));
                break;
            }
            case Message::Role::Tool: {
                json item = {
                    {"role", "tool"},
                    {"tool_call_id", message.tool_call_id},
                    {"content", message.text},
                };
                if (!message.tool_name.empty()) item["name"] = message.tool_name;
                messages.push_back(std::move(item));
                break;
            }
            case Message::Role::System:
                break;
        }
    }
    return messages;
}

}  // namespace

ChatResponse chat(const Config& cfg, const ChatRequest& req) {
    ChatResponse out;
    const std::string api_key = normalizeApiKey(cfg.openrouter_api_key);
    if (api_key.empty()) {
        out.error = "OpenRouter: missing API key (set it in Settings or via "
                    "OPENROUTER_API_KEY).";
        return out;
    }
    if (!api_key.starts_with("sk-or-v1-")) {
        out.error = "OpenRouter: invalid API key. Use an OpenRouter key "
                    "starting with 'sk-or-v1-' in Settings or "
                    "OPENROUTER_API_KEY; provider-native keys are not valid.";
        return out;
    }

    json body = {
        {"model", cfg.model},
        {"messages", messagesAsOpenAI(req)},
        {"stream", false},
    };
    if (cfg.max_tokens > 0) body["max_tokens"] = cfg.max_tokens;
    if (cfg.temperature >= 0.0) body["temperature"] = cfg.temperature;
    if (!req.tools.empty()) body["tools"] = toolsAsOpenAI(req.tools);

    const auto response = http::post(
        kOpenRouterUrl,
        {
            "Authorization: Bearer " + api_key,
            "Content-Type: application/json",
            "Accept: application/json",
            "X-Title: OVI-CODE",
        },
        body.dump());

    if (response.status == 0) {
        out.error = "Network error: " +
                    (response.error.empty() ? std::string("unknown")
                                            : response.error);
        return out;
    }
    if (response.status >= 400) {
        out.error = "OpenRouter API error " + std::to_string(response.status) +
                    ": " + extractApiError(response.body);
        return out;
    }

    json parsed;
    try {
        parsed = json::parse(response.body);
    } catch (const std::exception& error) {
        out.error = std::string("OpenRouter: invalid JSON (") + error.what() +
                    ")";
        return out;
    }
    if (!parsed.contains("choices") || !parsed["choices"].is_array() ||
        parsed["choices"].empty()) {
        out.error = "OpenRouter: response missing 'choices'";
        return out;
    }

    const auto& choice = parsed["choices"][0];
    const auto message = choice.value("message", json::object());
    out.raw_finish_reason = choice.value("finish_reason", "");
    if (message.contains("content") && message["content"].is_string()) {
        out.text = message["content"].get<std::string>();
    }
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const auto& item : message["tool_calls"]) {
            ToolCall call;
            call.id = item.value("id", "");
            if (call.id.empty()) call.id = genId();
            const auto function = item.value("function", json::object());
            call.name = function.value("name", "");
            if (function.contains("arguments")) {
                const auto& arguments = function["arguments"];
                if (arguments.is_string()) {
                    try {
                        call.input = json::parse(arguments.get<std::string>());
                    } catch (...) {
                        call.input = json::object();
                    }
                } else if (arguments.is_object()) {
                    call.input = arguments;
                }
            }
            if (call.input.is_null()) call.input = json::object();
            out.tool_calls.push_back(std::move(call));
        }
    }
    out.stop_requested = out.tool_calls.empty();
    return out;
}

}  // namespace ovicode
