#include "ovicode/agent_md.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace ovicode {

namespace fs = std::filesystem;

namespace {
std::string readIfExists(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || fs::is_directory(p, ec)) return {};
    std::ifstream f(p);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

AgentMd loadAgentMd(const fs::path& dir) {
    AgentMd md;
    md.source_dir = dir;
    md.agent_body = readIfExists(dir / "agent.md");
    md.tools_body = readIfExists(dir / "tools.md");
    md.skills_body = readIfExists(dir / "skills.md");
    return md;
}

std::string assembleSystemPrompt(const AgentMd& md, Technique technique,
                                 const fs::path& workspace) {
    std::ostringstream ss;
    ss << "You are ovi-code, a terminal-based code-editing agent. "
       << "You operate inside a workspace and use the provided tools "
       << "(`read_file`, `list_files`, `edit_file`) to inspect and modify "
       << "files. Be concise. Prefer reading before editing. "
       << "Make minimal, targeted changes. When a task is complete, reply with "
       << "a short summary instead of calling more tools.\n\n"
       << "Workspace: " << workspace.string() << "\n";

    switch (technique) {
        case Technique::Standard:
            break;
        case Technique::ChainOfThought:
            ss << "\n## Thinking style\n"
               << "Before each tool call, briefly think step by step about what "
               << "you need to know and why. Write your reasoning as a short "
               << "paragraph, then act.\n";
            break;
        case Technique::RalphLoop:
            ss << "\n## Ralph loop\n"
               << "You are being re-prompted with the same goal across several "
               << "iterations. On each turn, make one concrete improvement, "
               << "verify it, and stop. If the goal already appears complete, "
               << "say so explicitly with the token `DONE`.\n";
            break;
        case Technique::Rag:
            ss << "\n## Retrieval-augmented mode\n"
               << "Relevant excerpts from the workspace have been pre-loaded "
               << "into the conversation. Use them as hints but always verify "
               << "with `read_file` before editing.\n";
            break;
    }

    if (!md.agent_body.empty()) {
        ss << "\n## agent.md (workspace persona / rules)\n"
           << md.agent_body << "\n";
    }
    if (!md.tools_body.empty()) {
        ss << "\n## tools.md (tool-usage notes)\n" << md.tools_body << "\n";
    }
    if (!md.skills_body.empty()) {
        ss << "\n## skills.md (recipes / capabilities)\n"
           << md.skills_body << "\n";
    }
    return ss.str();
}

}  // namespace ovicode
