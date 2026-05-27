#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ovicode/agent_md.hpp"
#include "ovicode/config.hpp"
#include "ovicode/providers.hpp"

namespace ovicode {

struct TranscriptEntry {
    enum class Kind { User, Assistant, ToolCall, ToolResult, System, Error };
    Kind kind;
    std::string title;
    std::string body;
};

struct AgentContext {
    Config config;
    AgentMd md;
    std::filesystem::path workspace;
};

// Callback fired as new transcript entries are produced — lets the TUI render
// progressively. Return false to abort the run.
using TranscriptSink = std::function<bool(const TranscriptEntry&)>;

// Run a single user turn through the configured technique. Appends to
// `messages` so the same vector can be reused for the next turn.
void runTurn(const AgentContext& ctx,
             std::vector<Message>& messages,
             const std::string& user_input,
             const TranscriptSink& sink);

}  // namespace ovicode
