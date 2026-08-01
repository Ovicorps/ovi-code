#pragma once

#include <string>
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
    nlohmann::json reasoning_details;  // replayed for reasoning tool calls
    std::string tool_call_id;          // populated when role == Tool
    std::string tool_name;             // populated when role == Tool
    bool is_error = false;             // populated when role == Tool
};

struct ChatRequest {
    std::string system;
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
};

struct ChatResponse {
    std::string text;
    std::vector<ToolCall> tool_calls;
    nlohmann::json reasoning_details;
    bool stop_requested = false;   // model finished without tool calls
    std::string error;             // non-empty on transport / API error
    std::string raw_finish_reason;
};

ChatResponse chat(const Config& cfg, const ChatRequest& req);

}  // namespace ovicode
