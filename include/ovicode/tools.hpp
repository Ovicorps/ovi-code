#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ovicode {

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json input_schema;  // JSON Schema object
};

struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json input;
};

struct ToolResult {
    std::string tool_call_id;
    std::string tool_name;
    std::string content;
    bool is_error = false;
};

const std::vector<ToolDefinition>& builtinTools();

ToolResult executeTool(const std::filesystem::path& workspace, const ToolCall& call);

}  // namespace ovicode
