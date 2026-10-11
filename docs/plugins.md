# Technique plugins

Technique plugins let you extend how OVI-CODE handles a task without forking or
rebuilding the application. Each plugin is a native C++ dynamic library loaded
at process startup through a versioned, dependency-free C ABI. It appears in
setup, Settings, and the ++t++ technique cycle without linking against OVI-CODE
internals.

A plugin controls an agent turn while the host supplies the configured
provider, the existing ReAct/tool loop, and the conversation transcript. You
can start with a small prompt customization or build a multi-pass workflow
with its own settings and stopping criteria.

!!! danger "Plugins execute native code"
    A plugin has the same operating-system permissions as OVI-CODE. It is not
    sandboxed. Install only plugins whose source and publisher you trust.

## What you can extend

| Capability | How a plugin uses it | Example custom workflow |
| --- | --- | --- |
| Task instructions | Extend `host->base_system_prompt` before calling `host->run_react` | A code review guided by your team's conventions |
| Turn orchestration | Run multiple ReAct passes and inspect `host->last_assistant_text` between them | An implementation pass followed by critique and refinement |
| User-facing options | Declare typed V2 parameters and read them through `host->get_parameter` | Configurable review depth, extra instructions, or pass count |
| Progress reporting | Write System or Error entries with `host->emit` | Explain which workflow stage is running |

These are examples you can implement, not additional bundled techniques.
Plugins reuse the selected provider and host workspace tools rather than
requiring separate model-client or tool implementations.

The current public ABI extends **agentic techniques**; it does not provide a
registry for adding providers, workspace tools, or TUI panels. The workspace
sandbox applies to the host's file tools, not to a plugin's own native code.

## Bundled example

OVI-CODE ships with the **Deliberate** example plugin. It is installed and
loaded automatically, and fresh configurations select it as the default
technique so you can see a plugin working immediately. Existing configurations
keep their saved technique.

Deliberate adds explicit hypothesis and verification instructions before a
normal ReAct pass. Its **Verification depth** and **Show start message** options
are available under **Settings → Plugins**. You can select another technique at
any time under **Settings → Agent** or by pressing ++t++ in chat Normal mode.

## Discover plugins

Without an override, OVI-CODE searches:

1. The `plugins` directory beside the executable. Source builds and portable
   release archives keep the bundled Deliberate module here.
2. `<install-prefix>/lib/ovi-code/plugins` for CMake prefix installs.
3. `~/.ovi-code/plugins` for plugins installed by the user.

Use a flag or environment variable to replace that search list with one
directory. This also disables the bundled example for that run unless it is in
the selected directory:

```bash
ovi-code --plugin-dir /path/to/plugins
OVI_CODE_PLUGIN_DIR=/path/to/plugins ovi-code
```

List loaded modules and loader errors without opening the TUI:

```bash
ovi-code plugins
```

OVI-CODE loads `.dylib` and `.so` files once at startup. Restart after adding or
rebuilding a plugin.

## Install and use a plugin

1. Obtain or build a trusted plugin for your operating system and CPU
   architecture. Put its `.dylib` or `.so` file in `~/.ovi-code/plugins`, creating
   the directory if needed, or use a directory from the search list above.
2. Run `ovi-code plugins` to check that the module loads and resolve any reported
   errors.
3. Start or restart OVI-CODE. Select the plugin during setup, under
   **Settings → Agent**, or with ++t++ in chat Normal mode.
4. If the plugin declares options, customize them under **Settings → Plugins**.
   Values are saved per plugin and supplied to it during each turn.

## Build the bundled example

The repository includes `plugins/examples/deliberate.cpp`, built by default:

```bash
cmake -S . -B build
cmake --build build -j
./build/ovi-code plugins
./build/ovi-code
```

The build-tree plugin directory is discovered automatically. Set
`-DOVI_CODE_BUILD_EXAMPLE_PLUGINS=OFF` when configuring CMake to omit the
example from both the build and installation.

## Write a plugin

Plugins include the dependency-free public ABI header. Use the V2 descriptor
to declare settings that appear under **Settings → Plugins**:

```cpp
#include "ovicode/technique_plugin.h"

#include <string>

namespace {

int runTurn(void*, const OviCodeTechniqueHostV2* host,
            const char* user_input) {
    if (!host || !host->run_react || !user_input) return 0;

    std::string system = host->base_system_prompt;
    system += "\n\nCheck assumptions before every tool call.";
    return host->run_react(host->context, system.c_str(), user_input);
}

const char* const modes[] = {"quick", "careful"};
const OviCodePluginParameterV2 parameters[] = {
    {"mode", "Verification mode", "How thoroughly to check assumptions.",
     OVI_CODE_PLUGIN_PARAMETER_CHOICE, "quick", modes, 2},
    {"notes", "Extra instructions", "Additional text added to the prompt.",
     OVI_CODE_PLUGIN_PARAMETER_STRING, "", nullptr, 0},
};

const OviCodeTechniquePluginV2 plugin{
    OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION_V2,
    "verify-first",
    "Verify First",
    "Checks assumptions before acting",
    parameters,
    2,
    nullptr,
    nullptr,
    runTurn,
};

}  // namespace

extern "C" const OviCodeTechniquePluginV2*
ovi_code_get_technique_plugin_v2() {
    return &plugin;
}
```

=== "macOS"

    ```bash
    mkdir -p ~/.ovi-code/plugins
    c++ -std=c++20 -dynamiclib -I/path/to/ovi-code/include \
      verify_first.cpp -o ~/.ovi-code/plugins/verify-first.dylib
    ```

=== "Linux"

    ```bash
    mkdir -p ~/.ovi-code/plugins
    c++ -std=c++20 -shared -fPIC -I/path/to/ovi-code/include \
      verify_first.cpp -o ~/.ovi-code/plugins/verify-first.so
    ```

Plugin IDs must start with a lowercase letter or digit and contain only
lowercase letters, digits, `.`, `_`, or `-`. They must not duplicate another
plugin or built-in technique ID.

V1 plugins remain supported and continue exporting
`ovi_code_get_technique_plugin_v1`, but only V2 plugins can declare settings.

## Define settings

Each V2 parameter has a stable `key`, user-facing `label` and `description`, a
type, and a default value. Supported types are `STRING`, `SECRET`, `INTEGER`,
`NUMBER`, `BOOLEAN`, and `CHOICE`. Choice parameters also provide an array of
accepted values. Keys follow the same character rules as plugin IDs and must be
unique within the plugin.

Values are stored per plugin in `~/.ovi-code/config.json`. Read the effective
value during a turn with `host->get_parameter`:

```cpp
const char* mode = host->get_parameter(host->context, "mode");
if (mode && std::string(mode) == "careful") {
    // Enable the plugin's more thorough behavior.
}
```

The host validates values and falls back to the declared default if a manually
edited configuration contains an invalid value. Returned pointers remain valid
until `run_turn` returns. Secret parameters are masked in Settings, but are
stored in the same user-readable configuration file as provider keys.

## Design a multi-pass workflow

A technique can do more than prepend instructions. For a review-and-refine
plugin, for example:

1. Start with `host->base_system_prompt` to retain the project's instructions.
2. Call `host->run_react` with the user's request and your implementation
   instructions.
3. Read `host->last_assistant_text`, then request another pass with review or
   refinement instructions when needed.
4. Stop when your completion criteria are met or the pass budget is exhausted.

**Settings → Agent → Plugin passes** caps the number of host ReAct passes a
plugin can request in one turn; `host->iteration_limit` exposes that budget.
Each pass also uses the configured tool-call limit. Respect failed callback
returns: `run_react` returns `0` on an error or cancellation, and `emit` returns
`0` when the turn has been cancelled. Return control rather than continuing the
workflow after either failure.

Copy any assistant text you need to keep before the next `run_react` call,
because that call can invalidate the pointer returned by
`last_assistant_text`.

## Runtime interface

| Field or callback | Purpose |
| --- | --- |
| `id`, `label`, `description` | Metadata shown and persisted by OVI-CODE |
| `create` / `destroy` | Optional process-lifetime plugin state |
| `run_turn` | Entry point for each user turn |
| `host->workspace` | Absolute active workspace path |
| `host->base_system_prompt` | Standard prompt plus project instructions |
| `host->run_react` | Run one complete model/tool pass |
| `host->last_assistant_text` | Inspect the latest assistant response |
| `host->emit` | Write System or Error transcript entries |
| `host->iteration_limit` | Maximum host-enforced passes for the turn |
| `host->get_parameter` | Read a V2 plugin setting or its default |

Pointers supplied by the host are valid only during `run_turn`; copy data that
must be retained. Never allow C++ exceptions to cross the exported C ABI.

See the exact declarations in
[`technique_plugin.h`](https://github.com/Ovicorps/ovi-code/blob/main/include/ovicode/technique_plugin.h).
