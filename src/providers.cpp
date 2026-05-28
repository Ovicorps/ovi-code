#include "ovicode/providers.hpp"

#include <random>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "ovicode/http.hpp"

namespace ovicode {

using json = nlohmann::json;

namespace {

// -------------------------------------------------------------------------
// Shared helpers
// -------------------------------------------------------------------------

std::string genId(const std::string& prefix) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream ss;
    ss << prefix << "_" << std::hex << rng();
    return ss.str();
}

// Reduce a configured base URL to just the host root.
//
// Strips trailing slashes (so concatenation can't produce "//v1/...") and any
// pre-existing version/endpoint path the caller will append itself
// ("/v1", "/v1beta", "/v1alpha", "/openai/v1"). Without this, a stale config
// like "https://generativelanguage.googleapis.com/v1beta/models/foo" combined
// with the per-provider suffix yields a doubled path and a 404 from Google's
// edge with an empty body.
std::string trimBaseUrl(const std::string& s) {
    std::string out = s;
    while (!out.empty() && out.back() == '/') out.pop_back();
    static const char* const kSuffixes[] = {
        "/v1beta", "/v1alpha", "/v1", "/openai/v1",
    };
    for (const char* suf : kSuffixes) {
        std::string needle = suf;
        auto pos = out.find(needle);
        if (pos == std::string::npos) continue;
        // Only strip if it's a real path component (followed by '/' or end of
        // string) — guards against matching inside a hostname.
        size_t end = pos + needle.size();
        if (end == out.size() || out[end] == '/') {
            out.erase(pos);
            break;
        }
    }
    return out;
}

// Pull the most useful error string out of a provider's JSON error envelope.
// Anthropic:   { "error": { "type": ..., "message": "..." } }
// OpenAI:      { "error": { "message": "...", "type": ..., "code": ... } }
// Gemini:      { "error": { "code": ..., "message": "...", "status": ... } }
// Falls back to the raw body if no structured message is found.
std::string extractApiError(const std::string& body) {
    if (body.empty()) return "<empty response body>";
    try {
        auto j = json::parse(body);
        if (j.is_object() && j.contains("error")) {
            const auto& e = j["error"];
            if (e.is_object() && e.contains("message") &&
                e["message"].is_string()) {
                return e["message"].get<std::string>();
            }
            if (e.is_string()) return e.get<std::string>();
        }
        if (j.is_object() && j.contains("message") &&
            j["message"].is_string()) {
            return j["message"].get<std::string>();
        }
    } catch (...) {
        // Not JSON; fall through.
    }
    return body;
}

ChatResponse transportError(const http::Response& r) {
    ChatResponse out;
    out.error = "Network error: " + (r.error.empty() ? "unknown" : r.error);
    return out;
}

ChatResponse apiError(const char* label, const http::Response& r) {
    ChatResponse out;
    out.error = std::string(label) + " API error " +
                std::to_string(r.status) + ": " + extractApiError(r.body);
    return out;
}

// -------------------------------------------------------------------------
// Tool-schema translators
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
// Anthropic — https://docs.anthropic.com/en/api/messages
// POST {base_url}/v1/messages
// Headers: x-api-key, anthropic-version, content-type
// Parallel tool_results MUST be grouped under a single user message whose
// content array holds one tool_result block per call.
// -------------------------------------------------------------------------

ChatResponse callAnthropic(const Config& cfg, const ChatRequest& req) {
    if (cfg.api_key.empty()) {
        ChatResponse out;
        out.error = "Anthropic: missing API key (set it in Settings or via "
                    "OVI_CODE_API_KEY).";
        return out;
    }

    json messages = json::array();
    for (size_t i = 0; i < req.messages.size(); ++i) {
        const auto& m = req.messages[i];
        switch (m.role) {
            case Message::Role::User:
                messages.push_back({
                    {"role", "user"},
                    {"content", m.text},
                });
                break;
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
                        {"input", tc.input.is_null() ? json::object() : tc.input},
                    });
                }
                if (content.empty()) {
                    content.push_back({{"type", "text"}, {"text", ""}});
                }
                messages.push_back({{"role", "assistant"}, {"content", content}});
                break;
            }
            case Message::Role::Tool: {
                // Collapse this run of Tool messages into one user turn.
                json content = json::array();
                size_t j = i;
                while (j < req.messages.size() &&
                       req.messages[j].role == Message::Role::Tool) {
                    const auto& t = req.messages[j];
                    content.push_back({
                        {"type", "tool_result"},
                        {"tool_use_id", t.tool_call_id},
                        {"content", t.text},
                        {"is_error", t.is_error},
                    });
                    ++j;
                }
                messages.push_back({{"role", "user"}, {"content", content}});
                i = j - 1;
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
    if (cfg.temperature >= 0.0) body["temperature"] = cfg.temperature;
    if (!req.system.empty()) body["system"] = req.system;
    if (!req.tools.empty()) body["tools"] = toolsAsAnthropic(req.tools);

    auto r = http::post(
        trimBaseUrl(cfg.base_url) + "/v1/messages",
        {
            "x-api-key: " + cfg.api_key,
            "anthropic-version: 2023-06-01",
            "content-type: application/json",
            "accept: application/json",
        },
        body.dump());

    if (r.status == 0) return transportError(r);
    if (r.status >= 400) return apiError("Anthropic", r);

    ChatResponse out;
    json resp;
    try {
        resp = json::parse(r.body);
    } catch (const std::exception& e) {
        out.error = std::string("Anthropic: invalid JSON (") + e.what() + ")";
        return out;
    }
    out.raw_finish_reason = resp.value("stop_reason", "");
    if (resp.contains("content") && resp["content"].is_array()) {
        for (const auto& part : resp["content"]) {
            auto type = part.value("type", "");
            if (type == "text") {
                out.text += part.value("text", "");
            } else if (type == "tool_use") {
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

// -------------------------------------------------------------------------
// OpenAI-compatible — https://platform.openai.com/docs/api-reference/chat
// Also used for llama.cpp's OpenAI-compatible server. The llama.cpp server
// usually has no auth, so Authorization is only attached when an api_key is
// set.
// POST {base_url}/v1/chat/completions
// -------------------------------------------------------------------------

ChatResponse callOpenAICompatible(const Config& cfg, const ChatRequest& req,
                                  const char* label, bool require_key) {
    if (require_key && cfg.api_key.empty()) {
        ChatResponse out;
        out.error = std::string(label) +
                    ": missing API key (set it in Settings or via "
                    "OVI_CODE_API_KEY).";
        return out;
    }

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
                json msg = {{"role", "assistant"}};
                // OpenAI accepts null content when tool_calls are present, but
                // tolerates an empty string in all other cases.
                if (!m.text.empty()) {
                    msg["content"] = m.text;
                } else if (m.tool_calls.empty()) {
                    msg["content"] = "";
                } else {
                    msg["content"] = nullptr;
                }
                if (!m.tool_calls.empty()) {
                    json arr = json::array();
                    for (const auto& tc : m.tool_calls) {
                        arr.push_back({
                            {"id", tc.id.empty() ? genId("call") : tc.id},
                            {"type", "function"},
                            {"function", {
                                {"name", tc.name},
                                {"arguments", tc.input.is_null()
                                                  ? std::string("{}")
                                                  : tc.input.dump()},
                            }},
                        });
                    }
                    msg["tool_calls"] = arr;
                }
                messages.push_back(msg);
                break;
            }
            case Message::Role::Tool: {
                json msg = {
                    {"role", "tool"},
                    {"tool_call_id", m.tool_call_id},
                    {"content", m.text},
                };
                if (!m.tool_name.empty()) msg["name"] = m.tool_name;
                messages.push_back(msg);
                break;
            }
            case Message::Role::System:
                break;
        }
    }

    json body = {
        {"model", cfg.model},
        {"messages", messages},
        {"stream", false},
    };
    if (cfg.max_tokens > 0) body["max_tokens"] = cfg.max_tokens;
    if (cfg.temperature >= 0.0) body["temperature"] = cfg.temperature;
    if (!req.tools.empty()) body["tools"] = toolsAsOpenAI(req.tools);

    std::vector<std::string> headers{
        "content-type: application/json",
        "accept: application/json",
    };
    if (!cfg.api_key.empty()) {
        headers.push_back("Authorization: Bearer " + cfg.api_key);
    }

    auto r = http::post(trimBaseUrl(cfg.base_url) + "/v1/chat/completions",
                        headers, body.dump());

    if (r.status == 0) return transportError(r);
    if (r.status >= 400) return apiError(label, r);

    ChatResponse out;
    json resp;
    try {
        resp = json::parse(r.body);
    } catch (const std::exception& e) {
        out.error = std::string(label) + ": invalid JSON (" + e.what() + ")";
        return out;
    }
    if (!resp.contains("choices") || !resp["choices"].is_array() ||
        resp["choices"].empty()) {
        out.error = std::string(label) + ": response missing 'choices'";
        return out;
    }
    const auto& c0 = resp["choices"][0];
    const auto msg = c0.value("message", json::object());
    out.raw_finish_reason = c0.value("finish_reason", "");

    if (msg.contains("content") && msg["content"].is_string()) {
        out.text = msg["content"].get<std::string>();
    }
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        for (const auto& tc : msg["tool_calls"]) {
            ToolCall call;
            call.id = tc.value("id", "");
            if (call.id.empty()) call.id = genId("call");
            const auto fn = tc.value("function", json::object());
            call.name = fn.value("name", "");
            if (fn.contains("arguments")) {
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
            } else {
                call.input = json::object();
            }
            out.tool_calls.push_back(std::move(call));
        }
    }
    out.stop_requested = out.tool_calls.empty();
    return out;
}

// -------------------------------------------------------------------------
// Gemini — https://ai.google.dev/api/generate-content
// POST {base_url}/v1beta/models/{model}:generateContent
// Auth via x-goog-api-key header (newer, preferred) — older ?key= query
// param still works but logs the key into server access logs.
// -------------------------------------------------------------------------

ChatResponse callGemini(const Config& cfg, const ChatRequest& req) {
    if (cfg.api_key.empty()) {
        ChatResponse out;
        out.error = "Gemini: missing API key (set it in Settings or via "
                    "OVI_CODE_API_KEY).";
        return out;
    }

    json contents = json::array();
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
                        {"args", tc.input.is_null() ? json::object() : tc.input},
                    }}});
                }
                if (parts.empty()) parts.push_back({{"text", ""}});
                contents.push_back({{"role", "model"}, {"parts", parts}});
                break;
            }
            case Message::Role::Tool: {
                json parts = json::array();
                size_t j = i;
                while (j < req.messages.size() &&
                       req.messages[j].role == Message::Role::Tool) {
                    const auto& t = req.messages[j];
                    parts.push_back({{"functionResponse", {
                        {"name", t.tool_name},
                        {"response", {{"result", t.text}}},
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
    json gen = json::object();
    if (cfg.temperature >= 0.0) gen["temperature"] = cfg.temperature;
    if (cfg.max_tokens > 0) gen["maxOutputTokens"] = cfg.max_tokens;
    if (!gen.empty()) body["generationConfig"] = gen;

    std::string url = trimBaseUrl(cfg.base_url) + "/v1beta/models/" +
                      cfg.model + ":generateContent";
    auto r = http::post(url,
                        {
                            "content-type: application/json",
                            "accept: application/json",
                            "x-goog-api-key: " + cfg.api_key,
                        },
                        body.dump());

    if (r.status == 0) return transportError(r);
    if (r.status >= 400) return apiError("Gemini", r);

    ChatResponse out;
    json resp;
    try {
        resp = json::parse(r.body);
    } catch (const std::exception& e) {
        out.error = std::string("Gemini: invalid JSON (") + e.what() + ")";
        return out;
    }
    if (!resp.contains("candidates") || !resp["candidates"].is_array() ||
        resp["candidates"].empty()) {
        // Prompt was blocked by safety filters or the request is malformed.
        std::string detail;
        if (resp.contains("promptFeedback")) {
            detail = resp["promptFeedback"].dump();
        }
        out.error = "Gemini: no candidates returned" +
                    (detail.empty() ? std::string() : " (" + detail + ")");
        return out;
    }
    const auto& cand = resp["candidates"][0];
    out.raw_finish_reason = cand.value("finishReason", "");
    const auto content = cand.value("content", json::object());
    if (content.contains("parts") && content["parts"].is_array()) {
        for (const auto& part : content["parts"]) {
            if (part.contains("text") && part["text"].is_string()) {
                out.text += part["text"].get<std::string>();
            } else if (part.contains("functionCall")) {
                const auto& fc = part["functionCall"];
                ToolCall tc;
                // Gemini doesn't return tool-call ids — synthesize one so we
                // can correlate the follow-up tool_result.
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
        case Provider::Anthropic:
            return callAnthropic(cfg, req);
        case Provider::OpenAI:
            return callOpenAICompatible(cfg, req, "OpenAI", /*require_key=*/true);
        case Provider::Llamacpp:
            return callOpenAICompatible(cfg, req, "llama.cpp",
                                        /*require_key=*/false);
        case Provider::Gemini:
            return callGemini(cfg, req);
    }
    ChatResponse out;
    out.error = "Unknown provider.";
    return out;
}

}  // namespace ovicode
