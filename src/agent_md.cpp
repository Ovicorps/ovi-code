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
            // Few-shot chain-of-thought, after arXiv:2201.11903: instead of
            // demonstrating only <task, answer>, the exemplars demonstrate
            // <task, reasoning chain, answer> so the model learns to emit a
            // reasoning chain before acting. Exemplars are adapted from the
            // arithmetic ones in cpp-agent/cot.cpp to this code-editing domain.
            ss << R"(
## Thinking style (chain of thought)
Before each tool call, work through the task in a short reasoning chain, then
act. Follow the pattern shown in these examples:

Task: The build fails with "undefined reference to `parseConfig`".
Reasoning: An undefined reference is a link-time error, so a declaration is
visible but no definition is linked in. I should find where parseConfig is
declared to confirm its signature, then check whether the file that defines it
is part of the build. I'll read the header, search for a definition, and if it
is missing wire the source file into the build or implement it.
Answer: Read the header for the signature, grep for the definition, then add the
missing definition or its translation unit to the build.

Task: Rename the function `oldName` to `newName` across the project.
Reasoning: A rename must cover the definition, the declaration, and every call
site, or the build breaks. I should first list the files that mention oldName so
I know the full blast radius, then edit each occurrence, and finally confirm no
stale references remain before reporting done.
Answer: List files containing oldName, edit the definition/declaration/call
sites, then re-check that no occurrences are left.

Now apply the same pattern to the user's task: think step by step in a short
reasoning chain, then act with the tools.
)";
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
