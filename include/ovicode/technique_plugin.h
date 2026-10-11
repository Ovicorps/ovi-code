#pragma once

#include <stdint.h>

#define OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION 1u
#define OVI_CODE_TECHNIQUE_PLUGIN_SYMBOL "ovi_code_get_technique_plugin_v1"
#define OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION_V2 2u
#define OVI_CODE_TECHNIQUE_PLUGIN_SYMBOL_V2 "ovi_code_get_technique_plugin_v2"

#ifdef __cplusplus
extern "C" {
#endif

enum OviCodePluginMessageKindV1 {
    OVI_CODE_PLUGIN_MESSAGE_SYSTEM = 0,
    OVI_CODE_PLUGIN_MESSAGE_ERROR = 1,
};

// All pointers supplied by the host are valid only for the duration of the
// run_turn call. Plugins must copy any data they retain.
struct OviCodeTechniqueHostV1 {
    uint32_t api_version;
    void* context;
    const char* workspace;
    const char* base_system_prompt;
    int iteration_limit;

    // Adds a user message and runs one complete ReAct/tool pass. An empty
    // system_prompt uses base_system_prompt. Returns 1 on success, 0 on error
    // or cancellation.
    int (*run_react)(void* context, const char* system_prompt,
                     const char* user_input);

    // Returns the most recent assistant text, or an empty string. The pointer
    // remains valid until the next run_react call.
    const char* (*last_assistant_text)(void* context);

    // Writes a System or Error entry to the transcript. Returns 0 when the
    // active turn has been cancelled and the plugin should stop immediately.
    int (*emit)(void* context, enum OviCodePluginMessageKindV1 kind,
                const char* title, const char* body);
};

struct OviCodeTechniquePluginV1 {
    uint32_t api_version;
    const char* id;
    const char* label;
    const char* description;

    // create/destroy are optional. A null create function gives run_turn a
    // null plugin_state.
    void* (*create)(void);
    void (*destroy)(void* plugin_state);

    // Returns 1 when the turn completed, 0 on failure. Exceptions must never
    // cross this ABI boundary.
    int (*run_turn)(void* plugin_state,
                    const struct OviCodeTechniqueHostV1* host,
                    const char* user_input);
};

// V2 adds plugin-defined settings. Values are represented as UTF-8 strings at
// the ABI boundary; `type` tells the host how to render and validate them.
enum OviCodePluginParameterTypeV2 {
    OVI_CODE_PLUGIN_PARAMETER_STRING = 0,
    OVI_CODE_PLUGIN_PARAMETER_SECRET = 1,
    OVI_CODE_PLUGIN_PARAMETER_INTEGER = 2,
    OVI_CODE_PLUGIN_PARAMETER_NUMBER = 3,
    OVI_CODE_PLUGIN_PARAMETER_BOOLEAN = 4,
    OVI_CODE_PLUGIN_PARAMETER_CHOICE = 5,
};

struct OviCodePluginParameterV2 {
    const char* key;
    const char* label;
    const char* description;
    enum OviCodePluginParameterTypeV2 type;
    const char* default_value;

    // Required only for CHOICE parameters. Option values are also used as the
    // labels shown by the host.
    const char* const* options;
    uint32_t option_count;
};

struct OviCodeTechniqueHostV2 {
    uint32_t api_version;
    void* context;
    const char* workspace;
    const char* base_system_prompt;
    int iteration_limit;

    int (*run_react)(void* context, const char* system_prompt,
                     const char* user_input);
    const char* (*last_assistant_text)(void* context);
    int (*emit)(void* context, enum OviCodePluginMessageKindV1 kind,
                const char* title, const char* body);

    // Returns this plugin's configured value for `key`, including its default
    // when the user has not changed it. The pointer is valid until run_turn
    // returns. Returns null for an unknown key.
    const char* (*get_parameter)(void* context, const char* key);
};

struct OviCodeTechniquePluginV2 {
    uint32_t api_version;
    const char* id;
    const char* label;
    const char* description;
    const struct OviCodePluginParameterV2* parameters;
    uint32_t parameter_count;

    void* (*create)(void);
    void (*destroy)(void* plugin_state);
    int (*run_turn)(void* plugin_state,
                    const struct OviCodeTechniqueHostV2* host,
                    const char* user_input);
};

typedef const struct OviCodeTechniquePluginV1*
    (*OviCodeGetTechniquePluginV1)(void);
typedef const struct OviCodeTechniquePluginV2*
    (*OviCodeGetTechniquePluginV2)(void);

#ifdef __cplusplus
}
#endif
