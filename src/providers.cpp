#include "ovicode/providers.hpp"

#include <cctype>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ovicode/http.hpp"

namespace ovicode {

using json = nlohmann::json;

namespace {

constexpr const char* kOpenRouterUrl =
    "https://openrouter.ai/api/v1/chat/completions";
constexpr const char* kOpenAIUrl =
    "https://api.openai.com/v1/chat/completions";
constexpr const char* kAnthropicUrl = "https://api.anthropic.com/v1/messages";
constexpr const char* kXAIUrl = "https://api.x.ai/v1/chat/completions";

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
    std::ostringstream stream;
    stream << "call_" << std::hex << rng();
    return stream.str();
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

ChatResponse transportError(const http::Response& response) {
    ChatResponse out;
    if (response.cancelled) {
        out.cancelled = true;
        return out;
    }
    out.error = "Network error: " +
                (response.error.empty() ? std::string("unknown")
                                        : response.error);
    return out;
}

ChatResponse apiError(const std::string& label,
                      const http::Response& response) {
    ChatResponse out;
    out.error = label + " API error " + std::to_string(response.status) +
                ": " + extractApiError(response.body);
    return out;
}

json toolsAsOpenAI(std::span<const ToolDefinition> tools) {
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

json toolsAsAnthropic(std::span<const ToolDefinition> tools) {
    json result = json::array();
    for (const auto& tool : tools) {
        result.push_back({
            {"name", tool.name},
            {"description", tool.description},
            {"input_schema", tool.input_schema},
        });
    }
    return result;
}

json messagesAsOpenAI(const ChatRequest& req, bool replay_reasoning) {
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
                if (replay_reasoning && message.reasoning_details.is_array() &&
                    !message.reasoning_details.empty()) {
                    item["reasoning_details"] = message.reasoning_details;
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

ChatResponse parseOpenAIResponse(const std::string& label,
                                 const http::Response& response,
                                 bool preserve_reasoning) {
    if (response.status == 0) return transportError(response);
    if (response.status < 200 || response.status >= 300) {
        return apiError(label, response);
    }

    ChatResponse out;
    try {
        const json parsed = json::parse(response.body);
        if (!parsed.is_object() || !parsed.contains("choices") ||
            !parsed["choices"].is_array() || parsed["choices"].empty() ||
            !parsed["choices"][0].is_object()) {
            out.error = label + ": response missing 'choices'";
            return out;
        }

        const auto& choice = parsed["choices"][0];
        if (choice.contains("finish_reason") &&
            choice["finish_reason"].is_string()) {
            out.raw_finish_reason =
                choice["finish_reason"].get<std::string>();
        }
        if (!choice.contains("message") || !choice["message"].is_object()) {
            out.error = label + ": response missing 'message'";
            return out;
        }
        const auto& message = choice["message"];
        if (message.contains("content") && message["content"].is_string()) {
            out.text = message["content"].get<std::string>();
        } else if (message.contains("content") &&
                   message["content"].is_array()) {
            for (const auto& part : message["content"]) {
                if (!part.is_object()) continue;
                const std::string type =
                    part.contains("type") && part["type"].is_string()
                        ? part["type"].get<std::string>()
                        : std::string{};
                if (type == "text" && part.contains("text") &&
                    part["text"].is_string()) {
                    out.text += part["text"].get<std::string>();
                } else if (type == "refusal") {
                    if (part.contains("refusal") &&
                        part["refusal"].is_string()) {
                        out.text += part["refusal"].get<std::string>();
                    } else if (part.contains("text") &&
                               part["text"].is_string()) {
                        out.text += part["text"].get<std::string>();
                    }
                }
            }
        }
        if (message.contains("refusal") && message["refusal"].is_string()) {
            if (!out.text.empty()) out.text += "\n";
            out.text += message["refusal"].get<std::string>();
        }
        if (preserve_reasoning && message.contains("reasoning_details") &&
            message["reasoning_details"].is_array()) {
            out.reasoning_details = message["reasoning_details"];
        }
        if (message.contains("tool_calls") &&
            message["tool_calls"].is_array()) {
            for (const auto& item : message["tool_calls"]) {
                if (!item.is_object()) continue;
                ToolCall call;
                if (item.contains("id") && item["id"].is_string()) {
                    call.id = item["id"].get<std::string>();
                }
                if (call.id.empty()) call.id = genId();
                if (!item.contains("function") ||
                    !item["function"].is_object()) {
                    continue;
                }
                const auto& function = item["function"];
                if (function.contains("name") &&
                    function["name"].is_string()) {
                    call.name = function["name"].get<std::string>();
                }
                if (function.contains("arguments")) {
                    const auto& arguments = function["arguments"];
                    if (arguments.is_string()) {
                        try {
                            call.input =
                                json::parse(arguments.get<std::string>());
                        } catch (...) {
                            call.input = json::object();
                        }
                    } else if (arguments.is_object()) {
                        call.input = arguments;
                    }
                }
                if (call.input.is_null()) call.input = json::object();
                if (!call.name.empty()) out.tool_calls.push_back(std::move(call));
            }
        }
    } catch (const std::exception& error) {
        out = ChatResponse{};
        out.error = label + ": invalid response (" + error.what() + ")";
        return out;
    }
    if (out.raw_finish_reason == "length" ||
        out.raw_finish_reason == "content_filter") {
        out.error = label + ": incomplete response (finish_reason=" +
                    out.raw_finish_reason + ")";
        return out;
    }
    if (out.raw_finish_reason == "tool_calls" && out.tool_calls.empty()) {
        out.error = label + ": finish_reason=tool_calls without valid calls";
        return out;
    }
    if (!out.tool_calls.empty() && out.raw_finish_reason != "tool_calls") {
        out.error = label + ": tool calls with inconsistent finish_reason=" +
                    out.raw_finish_reason;
        return out;
    }
    if (out.tool_calls.empty() && out.raw_finish_reason != "stop") {
        out.error = label + ": unsupported finish_reason=" +
                    out.raw_finish_reason;
        return out;
    }
    if (out.text.empty() && out.tool_calls.empty()) {
        out.error = label + ": empty response";
        return out;
    }
    out.stop_requested = out.tool_calls.empty();
    return out;
}

json openAIRequestBody(const Config& cfg, const ChatRequest& req,
                       Provider provider) {
    const std::string& model = providerAccount(cfg, provider).model;
    json body = {
        {"model", model},
        {"messages", messagesAsOpenAI(req, provider == Provider::OpenRouter)},
        {"stream", false},
    };
    if (cfg.max_tokens > 0) {
        body[provider == Provider::OpenRouter ? "max_tokens"
                                              : "max_completion_tokens"] =
            cfg.max_tokens;
    }
    const ModelInfo* model_info = findModelInfo(provider, model);
    if (cfg.temperature >= 0.0 &&
        (!model_info || model_info->supports_temperature)) {
        body["temperature"] = cfg.temperature;
    }
    const ReasoningEffort effort =
        effectiveReasoningEffort(provider, model, cfg.reasoning_effort);
    if (effort != ReasoningEffort::Auto) {
        if (provider == Provider::OpenRouter) {
            body["reasoning"] = {
                {"effort", reasoningEffortToString(effort)},
            };
        } else {
            body["reasoning_effort"] = reasoningEffortToString(effort);
        }
    }
    if (!req.tools.empty()) body["tools"] = toolsAsOpenAI(req.tools);
    return body;
}

json messagesAsAnthropic(const ChatRequest& req) {
    json messages = json::array();
    for (size_t i = 0; i < req.messages.size(); ++i) {
        const auto& message = req.messages[i];
        switch (message.role) {
            case Message::Role::User:
                messages.push_back(
                    {{"role", "user"}, {"content", message.text}});
                break;
            case Message::Role::Assistant: {
                json content = message.reasoning_details.is_array() &&
                                       !message.reasoning_details.empty()
                                   ? message.reasoning_details
                                   : json::array();
                if (content.empty()) {
                    if (!message.text.empty()) {
                        content.push_back({{"type", "text"},
                                           {"text", message.text}});
                    }
                    for (const auto& call : message.tool_calls) {
                        content.push_back({
                            {"type", "tool_use"},
                            {"id", call.id.empty() ? genId() : call.id},
                            {"name", call.name},
                            {"input", call.input.is_null() ? json::object()
                                                           : call.input},
                        });
                    }
                }
                if (content.empty()) {
                    content.push_back({{"type", "text"}, {"text", ""}});
                }
                messages.push_back(
                    {{"role", "assistant"}, {"content", std::move(content)}});
                break;
            }
            case Message::Role::Tool: {
                json content = json::array();
                size_t next = i;
                while (next < req.messages.size() &&
                       req.messages[next].role == Message::Role::Tool) {
                    const auto& tool = req.messages[next];
                    content.push_back({
                        {"type", "tool_result"},
                        {"tool_use_id", tool.tool_call_id},
                        {"content", tool.text},
                        {"is_error", tool.is_error},
                    });
                    ++next;
                }
                messages.push_back(
                    {{"role", "user"}, {"content", std::move(content)}});
                i = next - 1;
                break;
            }
            case Message::Role::System:
                break;
        }
    }
    return messages;
}

json anthropicRequestBody(const Config& cfg, const ChatRequest& req) {
    const std::string& model = selectedModel(cfg);
    json body = {
        {"model", model},
        {"max_tokens", cfg.max_tokens > 0 ? cfg.max_tokens : 4096},
        {"messages", messagesAsAnthropic(req)},
    };
    if (!req.system.empty()) body["system"] = req.system;
    if (!req.tools.empty()) body["tools"] = toolsAsAnthropic(req.tools);
    const ModelInfo* model_info = findModelInfo(Provider::Anthropic, model);
    if (cfg.temperature >= 0.0 &&
        (!model_info || model_info->supports_temperature)) {
        body["temperature"] = cfg.temperature;
    }
    const ReasoningEffort effort = effectiveReasoningEffort(
        Provider::Anthropic, model, cfg.reasoning_effort);
    if (effort != ReasoningEffort::Auto) {
        body["thinking"] = {{"type", "adaptive"}};
        body["output_config"] = {
            {"effort", reasoningEffortToString(effort)},
        };
    }
    return body;
}

ChatResponse requestTooLarge(const Config& cfg, std::size_t actual) {
    ChatResponse out;
    out.error = "Request is " + std::to_string(actual) +
                " bytes, exceeding max_request_bytes (" +
                std::to_string(cfg.max_request_bytes) + ").";
    return out;
}

ChatResponse callOpenAICompatible(const Config& cfg, const ChatRequest& req,
                                   Provider provider, const std::string& url,
                                   std::stop_token stop) {
    const ProviderInfo& info = providerInfo(provider);
    const std::string api_key =
        normalizeApiKey(effectiveApiKey(cfg, provider));
    if (api_key.empty()) {
        ChatResponse out;
        out.error = info.label + ": missing API key (set it in Settings or via " +
                    info.environment_variable + ").";
        return out;
    }
    if (provider == Provider::OpenRouter &&
        !api_key.starts_with("sk-or-v1-")) {
        ChatResponse out;
        out.error = "OpenRouter: invalid API key. Use an OpenRouter key "
                    "starting with 'sk-or-v1-'.";
        return out;
    }

    const std::string body = openAIRequestBody(cfg, req, provider).dump();
    if (cfg.max_request_bytes > 0 && body.size() > cfg.max_request_bytes) {
        return requestTooLarge(cfg, body.size());
    }

    std::vector<std::string> headers{
        "Authorization: Bearer " + api_key,
        "Content-Type: application/json",
        "Accept: application/json",
    };
    if (provider == Provider::OpenRouter) headers.push_back("X-Title: OVI-CODE");
    const auto response = http::post(url, headers, body, 120, stop);
    return parseOpenAIResponse(info.label, response,
                               provider == Provider::OpenRouter);
}

ChatResponse callAnthropic(const Config& cfg, const ChatRequest& req,
                           std::stop_token stop) {
    const ProviderInfo& info = providerInfo(Provider::Anthropic);
    const std::string api_key =
        normalizeApiKey(effectiveApiKey(cfg, Provider::Anthropic));
    if (api_key.empty()) {
        ChatResponse out;
        out.error = "Anthropic: missing API key (set it in Settings or via " +
                    info.environment_variable + ").";
        return out;
    }

    const std::string body = anthropicRequestBody(cfg, req).dump();
    if (cfg.max_request_bytes > 0 && body.size() > cfg.max_request_bytes) {
        return requestTooLarge(cfg, body.size());
    }

    const auto response = http::post(
        kAnthropicUrl,
        {
            "x-api-key: " + api_key,
            "anthropic-version: 2023-06-01",
            "content-type: application/json",
            "accept: application/json",
        },
        body, 120, stop);
    if (response.status == 0) return transportError(response);
    if (response.status < 200 || response.status >= 300) {
        return apiError(info.label, response);
    }

    ChatResponse out;
    try {
        const json parsed = json::parse(response.body);
        if (!parsed.is_object() || !parsed.contains("content") ||
            !parsed["content"].is_array()) {
            out.error = "Anthropic: response missing 'content'";
            return out;
        }
        if (parsed.contains("stop_reason") &&
            parsed["stop_reason"].is_string()) {
            out.raw_finish_reason =
                parsed["stop_reason"].get<std::string>();
        }
        out.reasoning_details = parsed["content"];
        for (const auto& part : parsed["content"]) {
            if (!part.is_object() || !part.contains("type") ||
                !part["type"].is_string()) {
                continue;
            }
            const std::string type = part["type"].get<std::string>();
            if (type == "text") {
                if (part.contains("text") && part["text"].is_string()) {
                    out.text += part["text"].get<std::string>();
                }
            } else if (type == "tool_use") {
                ToolCall call;
                if (part.contains("id") && part["id"].is_string()) {
                    call.id = part["id"].get<std::string>();
                }
                if (call.id.empty()) call.id = genId();
                if (part.contains("name") && part["name"].is_string()) {
                    call.name = part["name"].get<std::string>();
                }
                call.input = part.contains("input") && part["input"].is_object()
                                 ? part["input"]
                                 : json::object();
                if (!call.name.empty()) out.tool_calls.push_back(std::move(call));
            }
        }
    } catch (const std::exception& error) {
        out = ChatResponse{};
        out.error = std::string("Anthropic: invalid response (") + error.what() +
                    ")";
        return out;
    }
    if (out.raw_finish_reason == "max_tokens" ||
        out.raw_finish_reason == "refusal" ||
        out.raw_finish_reason == "pause_turn") {
        out.error = "Anthropic: incomplete response (stop_reason=" +
                    out.raw_finish_reason + ")";
        return out;
    }
    if (out.raw_finish_reason == "tool_use" && out.tool_calls.empty()) {
        out.error = "Anthropic: stop_reason=tool_use without valid calls";
        return out;
    }
    if (!out.tool_calls.empty() && out.raw_finish_reason != "tool_use") {
        out.error = "Anthropic: tool calls with inconsistent stop_reason=" +
                    out.raw_finish_reason;
        return out;
    }
    if (out.tool_calls.empty() && out.raw_finish_reason != "end_turn" &&
        out.raw_finish_reason != "stop_sequence") {
        out.error = "Anthropic: unsupported stop_reason=" +
                    out.raw_finish_reason;
        return out;
    }
    if (out.text.empty() && out.tool_calls.empty()) {
        out.error = "Anthropic: empty response";
        return out;
    }
    out.stop_requested = out.tool_calls.empty();
    return out;
}

}  // namespace

std::string serializeChatRequestBody(const Config& cfg,
                                     const ChatRequest& req) {
    if (cfg.provider == Provider::Anthropic) {
        return anthropicRequestBody(cfg, req).dump();
    }
    return openAIRequestBody(cfg, req, cfg.provider).dump();
}

ChatResponse chat(const Config& cfg, const ChatRequest& req,
                  std::stop_token stop) {
    if (stop.stop_requested()) {
        ChatResponse out;
        out.cancelled = true;
        return out;
    }
    switch (cfg.provider) {
        case Provider::OpenRouter:
            return callOpenAICompatible(cfg, req, Provider::OpenRouter,
                                         kOpenRouterUrl, stop);
        case Provider::OpenAI:
            return callOpenAICompatible(cfg, req, Provider::OpenAI,
                                         kOpenAIUrl, stop);
        case Provider::Anthropic:
            return callAnthropic(cfg, req, stop);
        case Provider::XAI:
            return callOpenAICompatible(cfg, req, Provider::XAI, kXAIUrl,
                                        stop);
    }
    ChatResponse out;
    out.error = "Unknown provider.";
    return out;
}

}  // namespace ovicode
