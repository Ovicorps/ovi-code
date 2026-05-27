#include "ovicode/providers.hpp"

#include <chrono>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

#include "ovicode/http.hpp"

namespace ovicode {

using json = nlohmann::json;

namespace {

std::string genId(const std::string& prefix) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream ss;
    ss << prefix << "_" << std::hex << rng();
    return ss.str();
}

json toolsAsAnthropic(const std::vector<ToolDefinition>& tools) {
    json arr = json::array();
    for (const auto& t : tools) {
        arr.push_back({
            {"name", t.name},
            {"description", t.description},
            {"input_schema", t.input_schema},
        });
    }
    return arr;
}

json toolsAsOpenAI(const std::vector<ToolDefinition>& tools) {
    json arr = json::array();
    for (const auto& t : tools) {
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name", t.name},
                {"description", t.description},
                {"parameters", t.input_schema},
            }},
        });
    }
    return arr;
}

json toolsAsGemini(const std::vector<ToolDefinition>& tools) {
    json decls = json::array();
    for (const auto& t : tools) {
        decls.push_back({
            {"name", t.name},
            {"description", t.description},
            {"parameters", t.input_schema},
        });
    }
    return json::array({json{{"functionDeclarations", decls}}});
}

// ----- Anthropic ----------------------------------------------------------

ChatResponse callAnthropic(const Config& cfg, const ChatRequest& req) {
    ChatResponse out;
    json messages = json::array();
    for (const auto& m : req.messages) {
        switch (m.role) {
            case Message::Role::User: {
                messages.push_back({{"role", "user"}, {"content", m.text}});
                break;
            }
            case Message::Role::Assistant: {
                json content = json::array();
                if (!m.text.empty()) {
                    content.push_back({{"type", "text"}, {"text", m.text}});
                }
                for (const auto& tc : m.tool_calls) {
                    content.push_back({
                        {"type", "tool_use"},
                        {"id", tc.id},
                        {"name", tc.name},
                        {"input", tc.input},
                    });
                }
                messages.push_back({{"role", "assistant"}, {"content", content}});
                break;
            }
            case Message::Role::Tool: {
                json content = json::array({json{
                    {"type", "tool_result"},
                    {"tool_use_id", m.tool_call_id},
                    {"content", m.text},
                    {"is_error", m.is_error},
                }});
                messages.push_back({{"role", "user"}, {"content", content}});
                break;
            }
            case Message::Role::System:
                break;  // handled separately
        }
    }

    json body = {
        {"model", cfg.model},
        {"max_tokens", cfg.max_tokens},
        {"messages", messages},
    };
    if (!req.system.empty()) body["system"] = req.system;
    if (!req.tools.empty()) body["tools"] = toolsAsAnthropic(req.tools);

    auto r = http::post(
        cfg.base_url + "/v1/messages",
        {
            "x-api-key: " + cfg.api_key,
            "anthropic-version: 2023-06-01",
            "content-type: application/json",
        },
        body.dump());

    if (r.status == 0) {
        out.error = "Network error: " + r.error;
        return out;
    }
    if (r.status >= 400) {
        out.error = "Anthropic API error " + std::to_string(r.status) + ": " +
                    r.body;
        return out;
    }
    json resp;
    try {
        resp = json::parse(r.body);
    } catch (const std::exception& e) {
        out.error = std::string("Bad JSON from Anthropic: ") + e.what();
        return out;
    }
    out.raw_finish_reason = resp.value("stop_reason", "");
    if (resp.contains("content") && resp["content"].is_array()) {
        for (const auto& part : resp["content"]) {
            auto t = part.value("type", "");
            if (t == "text") {
                out.text += part.value("text", "");
            } else if (t == "tool_use") {
                ToolCall tc;
                tc.id = part.value("id", "");
                tc.name = part.value("name", "");
                tc.input = part.value("input", json::object());
                out.tool_calls.push_back(std::move(tc));
            }
        }
    }
    out.stop_requested = out.tool_calls.empty();
    return out;
}

// ----- OpenAI / Ollama (chat completions) ---------------------------------

ChatResponse callOpenAILike(const Config& cfg, const ChatRequest& req,
                            bool is_ollama) {
    ChatResponse out;
    json messages = json::array();
    if (!req.system.empty()) {
        messages.push_back({{"role", "system"}, {"content", req.system}});
    }
    for (const auto& m : req.messages) {
        switch (m.role) {
            case Message::Role::User:
                messages.push_back({{"role", "user"}, {"content", m.text}});
                break;
            case Message::Role::Assistant: {
                json msg = {{"role", "assistant"}, {"content", m.text}};
                if (!m.tool_calls.empty()) {
                    json arr = json::array();
                    for (const auto& tc : m.tool_calls) {
                        arr.push_back({
                            {"id", tc.id},
                            {"type", "function"},
                            {"function", {
                                {"name", tc.name},
                                {"arguments", tc.input.dump()},
                            }},
                        });
                    }
                    msg["tool_calls"] = arr;
                }
                messages.push_back(msg);
                break;
            }
            case Message::Role::Tool:
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", m.tool_call_id},
                    {"name", m.tool_name},
                    {"content", m.text},
                });
                break;
            case Message::Role::System:
                break;
        }
    }
    json body = {
        {"model", cfg.model},
        {"messages", messages},
        {"temperature", cfg.temperature},
    };
    if (!req.tools.empty()) body["tools"] = toolsAsOpenAI(req.tools);
    if (is_ollama) body["stream"] = false;

    std::string url = cfg.base_url +
                      (is_ollama ? "/api/chat" : "/v1/chat/completions");
    std::vector<std::string> headers{"content-type: application/json"};
    if (!is_ollama && !cfg.api_key.empty()) {
        headers.push_back("Authorization: Bearer " + cfg.api_key);
    }
    auto r = http::post(url, headers, body.dump());

    if (r.status == 0) {
        out.error = "Network error: " + r.error;
        return out;
    }
    if (r.status >= 400) {
        out.error = std::string(is_ollama ? "Ollama" : "OpenAI") +
                    " API error " + std::to_string(r.status) + ": " + r.body;
        return out;
    }
    json resp;
    try {
        resp = json::parse(r.body);
    } catch (const std::exception& e) {
        out.error = std::string("Bad JSON: ") + e.what();
        return out;
    }
    json msg;
    if (is_ollama) {
        msg = resp.value("message", json::object());
        out.raw_finish_reason = resp.value("done_reason", "");
    } else {
        if (!resp.contains("choices") || !resp["choices"].is_array() ||
            resp["choices"].empty()) {
            out.error = "Missing choices in response.";
            return out;
        }
        auto& c0 = resp["choices"][0];
        msg = c0.value("message", json::object());
        out.raw_finish_reason = c0.value("finish_reason", "");
    }

    if (msg.contains("content") && msg["content"].is_string()) {
        out.text = msg["content"].get<std::string>();
    }
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
            ToolCall call;
            call.id = tc.value("id", "");
            if (call.id.empty()) call.id = genId("call");
            const auto& fn = tc.value("function", json::object());
            call.name = fn.value("name", "");
            const auto& args = fn["arguments"];
            if (args.is_string()) {
                try {
                    call.input = json::parse(args.get<std::string>());
                } catch (...) {
                    call.input = json::object();
                }
            } else if (args.is_object()) {
                call.input = args;
            } else {
                call.input = json::object();
            }
            out.tool_calls.push_back(std::move(call));
        }
    }
    out.stop_requested = out.tool_calls.empty();
    return out;
}

// ----- Gemini -------------------------------------------------------------

ChatResponse callGemini(const Config& cfg, const ChatRequest& req) {
    ChatResponse out;
    json contents = json::array();

    // Walk messages, grouping any consecutive Tool messages into a single user
    // turn with multiple functionResponse parts.
    for (size_t i = 0; i < req.messages.size(); ++i) {
        const auto& m = req.messages[i];
        switch (m.role) {
            case Message::Role::User:
                contents.push_back({
                    {"role", "user"},
                    {"parts", json::array({json{{"text", m.text}}})},
                });
                break;
            case Message::Role::Assistant: {
                json parts = json::array();
                if (!m.text.empty()) parts.push_back({{"text", m.text}});
                for (const auto& tc : m.tool_calls) {
                    parts.push_back({{"functionCall", {
                        {"name", tc.name},
                        {"args", tc.input},
                    }}});
                }
                contents.push_back({{"role", "model"}, {"parts", parts}});
                break;
            }
            case Message::Role::Tool: {
                json parts = json::array();
                size_t j = i;
                while (j < req.messages.size() &&
                       req.messages[j].role == Message::Role::Tool) {
                    parts.push_back({{"functionResponse", {
                        {"name", req.messages[j].tool_name},
                        {"response", {{"result", req.messages[j].text}}},
                    }}});
                    ++j;
                }
                contents.push_back({{"role", "user"}, {"parts", parts}});
                i = j - 1;
                break;
            }
            case Message::Role::System:
                break;
        }
    }

    json body = {{"contents", contents}};
    if (!req.system.empty()) {
        body["systemInstruction"] = {
            {"parts", json::array({json{{"text", req.system}}})},
        };
    }
    if (!req.tools.empty()) body["tools"] = toolsAsGemini(req.tools);
    body["generationConfig"] = {
        {"temperature", cfg.temperature},
        {"maxOutputTokens", cfg.max_tokens},
    };

    std::string url = cfg.base_url + "/v1beta/models/" + cfg.model +
                      ":generateContent?key=" + cfg.api_key;
    auto r = http::post(url, {"content-type: application/json"}, body.dump());

    if (r.status == 0) {
        out.error = "Network error: " + r.error;
        return out;
    }
    if (r.status >= 400) {
        out.error = "Gemini API error " + std::to_string(r.status) + ": " +
                    r.body;
        return out;
    }
    json resp;
    try {
        resp = json::parse(r.body);
    } catch (const std::exception& e) {
        out.error = std::string("Bad JSON from Gemini: ") + e.what();
        return out;
    }
    if (!resp.contains("candidates") || !resp["candidates"].is_array() ||
        resp["candidates"].empty()) {
        out.error = "Gemini returned no candidates: " + r.body;
        return out;
    }
    const auto& cand = resp["candidates"][0];
    out.raw_finish_reason = cand.value("finishReason", "");
    const auto& content = cand.value("content", json::object());
    if (content.contains("parts") && content["parts"].is_array()) {
        for (const auto& part : content["parts"]) {
            if (part.contains("text") && part["text"].is_string()) {
                out.text += part["text"].get<std::string>();
            } else if (part.contains("functionCall")) {
                const auto& fc = part["functionCall"];
                ToolCall tc;
                tc.id = genId("call");
                tc.name = fc.value("name", "");
                tc.input = fc.value("args", json::object());
                out.tool_calls.push_back(std::move(tc));
            }
        }
    }
    out.stop_requested = out.tool_calls.empty();
    return out;
}

}  // namespace

ChatResponse chat(const Config& cfg, const ChatRequest& req) {
    switch (cfg.provider) {
        case Provider::Anthropic: return callAnthropic(cfg, req);
        case Provider::OpenAI:    return callOpenAILike(cfg, req, false);
        case Provider::Ollama:    return callOpenAILike(cfg, req, true);
        case Provider::Gemini:    return callGemini(cfg, req);
    }
    ChatResponse out;
    out.error = "Unknown provider.";
    return out;
}

}  // namespace ovicode
