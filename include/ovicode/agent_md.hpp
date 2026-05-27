#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ovicode/config.hpp"

namespace ovicode {

struct AgentMd {
    std::string agent_body;    // contents of agent.md (system-prompt overrides / persona)
    std::string tools_body;    // contents of tools.md (extra tool guidance)
    std::string skills_body;   // contents of skills.md (recipes, capabilities)
    std::filesystem::path source_dir;
};

AgentMd loadAgentMd(const std::filesystem::path& dir);

std::string assembleSystemPrompt(const AgentMd& md, Technique technique,
                                 const std::filesystem::path& workspace);

}  // namespace ovicode
