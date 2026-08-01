#include "ovicode/technique_plugin.h"

#include <string>

namespace {

int runTurn(void*, const OviCodeTechniqueHostV1* host,
            const char* user_input) {
    if (!host || host->api_version != OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION ||
        !host->run_react || !user_input) {
        return 0;
    }

    std::string system = host->base_system_prompt
                             ? host->base_system_prompt
                             : "";
    system += R"(

## Deliberate technique
Before using a tool, state one concise hypothesis and the observation that
would confirm or reject it. After each tool result, update the hypothesis.
Prefer the smallest verified change and summarize the evidence at the end.
)";

    if (host->emit &&
        !host->emit(host->context, OVI_CODE_PLUGIN_MESSAGE_SYSTEM,
                    "Deliberate technique",
                    "Running the example runtime technique plugin.")) {
        return 0;
    }
    return host->run_react(host->context, system.c_str(), user_input);
}

const OviCodeTechniquePluginV1 kPlugin{
    OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION,
    "deliberate",
    "Deliberate",
    "Hypothesis-driven ReAct with explicit verification",
    nullptr,
    nullptr,
    runTurn,
};

}  // namespace

extern "C" const OviCodeTechniquePluginV1*
ovi_code_get_technique_plugin_v1() {
    return &kPlugin;
}
