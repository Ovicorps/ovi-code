#pragma once

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "ovicode/agent_md.hpp"
#include "ovicode/config.hpp"
#include "ovicode/plugins.hpp"
#include "ovicode/providers.hpp"
#include "ovicode/transcript.hpp"

namespace ovicode {

using ChatHandler = std::function<ChatResponse(
    const Config&, const ChatRequest&, std::stop_token)>;

struct AgentContext {
    Config config;
    AgentMd md;
    std::filesystem::path workspace;
    const TechniquePluginRegistry* plugins = nullptr;
    ChatHandler chat_handler = chat;
    int workspace_fd = -1;  // trusted session root; owned by the TUI
};

enum class TurnOutcome { Completed, Failed, Cancelled };

// Callback fired as new transcript entries are produced — lets the TUI render
// progressively. Return false to abort the run.
using TranscriptSink = std::function<bool(const TranscriptEntry&)>;

// Run a single user turn through the configured technique. Appends to
// `messages` so the same vector can be reused for the next turn.
TurnOutcome runTurn(const AgentContext& ctx,
                    std::vector<Message>& messages,
                    const std::string& user_input,
                    const TranscriptSink& sink,
                    std::stop_token stop = {});

}  // namespace ovicode
