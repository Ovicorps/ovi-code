# Technique plugins

Technique plugins are native C++ dynamic libraries loaded at process startup.
They appear in setup, Settings, and the ++t++ technique cycle without linking
against OVI-CODE internals.

!!! danger "Plugins execute native code"
    A plugin has the same operating-system permissions as OVI-CODE. It is not
    sandboxed. Install only plugins whose source and publisher you trust.

## Discover plugins

The default plugin directory is:

```text
~/.ovi-code/plugins
```

Override it with a flag or environment variable:

```bash
ovi-code --plugin-dir /path/to/plugins
OVI_CODE_PLUGIN_DIR=/path/to/plugins ovi-code
```

List loaded modules and loader errors without opening the TUI:

```bash
ovi-code plugins
ovi-code plugins --plugin-dir ./build/plugins
```

OVI-CODE loads `.dylib` and `.so` files once at startup. Restart after adding or
rebuilding a plugin.

## Build the example

The repository includes `plugins/examples/deliberate.cpp`, built by default:

```bash
cmake -S . -B build
cmake --build build -j
./build/ovi-code plugins --plugin-dir ./build/plugins
./build/ovi-code --plugin-dir ./build/plugins
```

## Write a plugin

Plugins include the dependency-free public ABI header:

```cpp
#include "ovicode/technique_plugin.h"

#include <string>

namespace {

int runTurn(void*, const OviCodeTechniqueHostV1* host,
            const char* user_input) {
    if (!host || !host->run_react || !user_input) return 0;

    std::string system = host->base_system_prompt;
    system += "\n\nCheck assumptions before every tool call.";
    return host->run_react(host->context, system.c_str(), user_input);
}

const OviCodeTechniquePluginV1 plugin{
    OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION,
    "verify-first",
    "Verify First",
    "Checks assumptions before acting",
    nullptr,
    nullptr,
    runTurn,
};

}  // namespace

extern "C" const OviCodeTechniquePluginV1*
ovi_code_get_technique_plugin_v1() {
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

Pointers supplied by the host are valid only during `run_turn`; copy data that
must be retained. Never allow C++ exceptions to cross the exported C ABI.

See the exact declarations in
[`technique_plugin.h`](https://github.com/Ovicorps/ovi-code/blob/main/include/ovicode/technique_plugin.h).
