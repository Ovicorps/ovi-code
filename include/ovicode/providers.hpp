#pragma once

#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ovicode/config.hpp"
#include "ovicode/tools.hpp"

namespace ovicode {

struct Message {
    enum class Role { System, User, Assistant, Tool };
    Role role;
    std::string text;
    std::vector<ToolCall> tool_calls;  // populated when role == Assistant
    // Opaque provider-native assistant state replayed during tool continuations.
    nlohmann::json reasoning_details;
    std::string tool_call_id;          // populated when role == Tool
    std::string tool_name;             // populated when role == Tool
    bool is_error = false;             // populated when role == Tool
};

struct ChatRequest {
    std::string_view system;
    std::span<const Message> messages;
    std::span<const ToolDefinition> tools;
};

struct ChatResponse {
    std::string text;
    std::vector<ToolCall> tool_calls;
    nlohmann::json reasoning_details;
    bool stop_requested = false;   // model finished without tool calls
    bool cancelled = false;
    std::string error;             // non-empty on transport / API error
    std::string raw_finish_reason;
};

ChatResponse chat(const Config& cfg, const ChatRequest& req,
                  std::stop_token stop = {});
std::string serializeChatRequestBody(const Config& cfg,
                                     const ChatRequest& req);

}  // namespace ovicode
