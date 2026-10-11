#include "ovicode/technique_plugin.h"

#include <string>

namespace {

int runTurn(void*, const OviCodeTechniqueHostV2* host,
            const char* user_input) {
    if (!host ||
        host->api_version != OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION_V2 ||
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

    const char* depth = host->get_parameter
                            ? host->get_parameter(host->context, "depth")
                            : nullptr;
    if (depth && std::string(depth) == "thorough") {
        system += "\nFor each hypothesis, consider at least one competing "
                  "explanation before acting.";
    }

    const char* announce = host->get_parameter
                               ? host->get_parameter(host->context, "announce")
                               : nullptr;
    if ((!announce || std::string(announce) == "true") && host->emit &&
        !host->emit(host->context, OVI_CODE_PLUGIN_MESSAGE_SYSTEM,
                    "Deliberate technique",
                    "Running the example runtime technique plugin.")) {
        return 0;
    }
    return host->run_react(host->context, system.c_str(), user_input);
}

const char* const kDepthOptions[] = {"brief", "thorough"};

const OviCodePluginParameterV2 kParameters[] = {
    {"depth", "Verification depth",
     "Choose how deeply the technique evaluates competing explanations.",
     OVI_CODE_PLUGIN_PARAMETER_CHOICE, "brief", kDepthOptions, 2},
    {"announce", "Show start message",
     "Write a transcript message when the technique starts.",
     OVI_CODE_PLUGIN_PARAMETER_BOOLEAN, "true", nullptr, 0},
};

const OviCodeTechniquePluginV2 kPlugin{
    OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION_V2,
    "deliberate",
    "Deliberate",
    "Hypothesis-driven ReAct with explicit verification",
    kParameters,
    2,
    nullptr,
    nullptr,
    runTurn,
};

}  // namespace

extern "C" const OviCodeTechniquePluginV2*
ovi_code_get_technique_plugin_v2() {
    return &kPlugin;
}
