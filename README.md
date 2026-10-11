![OVI-CODE logo](docs/assets/OviCode.png)

# OVI-CODE

OVI-CODE is a terminal-based coding agent written in C++20. It connects to
OpenRouter, OpenAI, Anthropic, or xAI/Grok, inspects a selected workspace, and
can read, create, and edit files while showing every tool call in a full-screen
TUI.

**Documentation:** [ovicorps.github.io/ovi-code](https://ovicorps.github.io/ovi-code/)

## Contents

- [Quick Start](#quick-start)
- [Using The TUI](#using-the-tui)
- [Agentic Techniques](#agentic-techniques)
- [Technique Plugins](#technique-plugins)
- [Curated Models And Effort](#curated-models-and-effort)
- [Project Instructions](#project-instructions)
- [Command Reference](#command-reference)
- [Tools And Safety](#tools-and-safety)
- [Configuration And Privacy](#configuration-and-privacy)
- [Troubleshooting](#troubleshooting)
- [Uninstall](#uninstall)
- [Building From Source](#building-from-source)

## Features

- Full-screen, keyboard-driven terminal interface built with FTXUI.
- Vim-style multiline prompt and configuration-file editors.
- Per-provider accounts for OpenRouter, OpenAI, Anthropic, and xAI/Grok.
- Provider-specific model pickers with capability-aware reasoning effort and
  support for custom model IDs.
- Visible model responses, tool calls, tool results, and file diffs.
- Automatically saved sessions, including unfinished prompts, with workspace-specific resume.
- Workspace-sandboxed file tools.
- Standard ReAct, Chain of Thought, Ralph Loop, and RAG techniques.
- Runtime-loaded C++ modules for user-defined agentic techniques, including a
  bundled configurable Deliberate example.
- Project instructions through `agent.md`, `tools.md`, and `skills.md`.
- Dark, light, system, and high-contrast plain themes.
- Native Apple Silicon and Intel Homebrew cask packaging.

## Requirements

To use OVI-CODE, you need:

- macOS or another environment capable of building the C++ application.
- A terminal with UTF-8 and color support.
- An API key and sufficient credits for at least one supported provider:
  OpenRouter, OpenAI, Anthropic, or xAI.

Building from source additionally requires CMake 3.20+, a C++20 compiler,
system libcurl, Git, and an internet connection during the first configure.

## Quick Start

### 1. Install

Install the prebuilt macOS cask after the Ovicorps tap has been published:

```bash
brew update
brew install --cask Ovicorps/ovi-code/ovi-code
```

The Homebrew tap is synchronized automatically with GitHub's latest stable
release. Run `brew update` before installing to refresh cached tap metadata.
For an existing installation, use `brew upgrade --cask Ovicorps/ovi-code/ovi-code`.

Alternatively, build from source:

```bash
git clone https://github.com/Ovicorps/ovi-code.git
cd ovi-code
cmake -S . -B build
cmake --build build -j
```

The source-built executable is `build/ovi-code`.

### 2. Configure

Launch the installed application:

```bash
ovi-code
```

For a source build, run:

```bash
./build/ovi-code
```

The first launch opens a five-step setup wizard:

1. Select OpenRouter, OpenAI, Anthropic, or xAI/Grok.
2. Enter that provider's API key.
3. Select a curated model or enter a custom provider model ID.
4. Select a reasoning effort supported by that model.
5. Select the default agentic technique.

The bundled **Deliberate** example plugin is preselected for fresh
configurations. Choose another technique during setup if preferred. Deliberate
can later be configured under **Settings → Plugins**.

The configuration is saved to `~/.ovi-code/config.json` with permissions set
to `0600`.

You can also provide provider API keys through the environment:

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
export OPENAI_API_KEY="sk-..."
export ANTHROPIC_API_KEY="sk-ant-..."
export XAI_API_KEY="xai-..."
ovi-code
```

### 3. Open A Project

Run OVI-CODE from a project directory:

```bash
cd ~/Projects/my-project
ovi-code
```

Or select a workspace explicitly:

```bash
ovi-code --workspace ~/Projects/my-project
```

The workspace is the security boundary for file tools. OVI-CODE refuses tool
access outside that directory.

### 4. Send A Request

The chat opens in Insert mode. Type a request, press `Esc` to enter Normal
mode, then press `Enter` to send it.

Example requests:

```text
Explain how authentication works in this project.
```

```text
Find the cause of the build failure, fix it, and rebuild the project.
```

```text
Rename parseConfig to loadConfig and update every call site.
```

Review tool calls and diffs in the output panel before accepting significant
changes into version control.

### How A Turn Works

1. OVI-CODE combines your request with the selected technique and project
   instruction files.
2. The model can answer directly or request a workspace tool.
3. Requested tools run locally and their results appear in the output panel.
4. Tool results are returned to the model until it produces a final response or
   reaches the configured tool-iteration limit.

Prompts, responses, tool activity, model context, and unfinished input are saved
automatically. Reopen OVI-CODE in the same workspace to resume the latest session.

### Saved Sessions

```bash
ovi-code                       # Resume the latest session in this workspace
ovi-code --new-session         # Start fresh without deleting earlier sessions
ovi-code sessions              # List saved session IDs, newest first
ovi-code --resume <id>         # Reopen a specific session
```

An interrupted turn restores its prompt unless a newer draft exists. Nothing
is sent and no tools are rerun until you submit a prompt. File changes made
before the interruption may already be present; review them before continuing.
If the provider or model has changed, the transcript is restored but model
context starts fresh.

## Using The TUI

OVI-CODE has two main panels:

- **output** shows the conversation, tool activity, errors, and diffs.
- **input** contains the multiline Vim-style prompt editor.

The highlighted panel name indicates which panel currently has keyboard focus.
Press `Ctrl+W` in Normal mode to switch between the input and output panels.

### Insert Mode

Insert mode edits the current prompt.

| Key | Action |
| --- | --- |
| Characters | Insert text |
| `Enter` | Insert a newline |
| `Tab` | Insert four spaces |
| Arrow keys | Move the cursor |
| `Home` / `End` | Move to the start / end of the line |
| `Backspace` / `Delete` | Delete text |
| `Esc` | Switch to Normal mode |

### Normal Mode

Normal mode provides Vim motions, editing commands, and application actions.

| Key | Action |
| --- | --- |
| `h` / `j` / `k` / `l` | Move left / down / up / right |
| `w` / `b` | Move forward / backward by word |
| `0` / `$` | Move to the start / end of the line |
| `gg` / `G` | Move to the first / last line |
| `i` / `a` | Insert before / after the cursor |
| `I` / `A` | Insert at the start / end of the line |
| `o` / `O` | Open a line below / above |
| `x` | Delete the character under the cursor |
| `dd` | Delete the current line |
| `yy` | Copy the current line |
| `p` / `P` | Paste after / before the current line |
| `Enter` | Send the prompt |
| `Ctrl+W` | Switch between input and output |
| `z` | Minimize or restore the input panel |
| `s` | Open Settings |
| `t` | Cycle the agentic technique |
| `c` | Clear the visible transcript |
| `q` | Quit OVI-CODE |

Copy and paste actions display a `COPIED`, `PASTED`, or `NOTHING TO PASTE`
indicator. The internal line clipboard is shared by the prompt, `tools.md`, and
`skills.md` editors.

### Output Panel

After pressing `Ctrl+W`, the output panel accepts these commands:

| Key | Action |
| --- | --- |
| `j` / `k` | Scroll down / up |
| `g` / `G` | Jump to the top / bottom |
| `Ctrl+W` | Return focus to the input panel |
| `s` | Open Settings |
| `t` | Cycle the agentic technique |
| `c` | Clear the visible transcript |
| `q` | Quit OVI-CODE |

### Settings

Press `s` from Normal mode to open Settings. **Save changes** writes settings
immediately and applies them to the next agent turn. The active config file is
reloaded before opening Settings and before each turn, so file-edited API keys
take effect without restarting. Saving preserves externally changed fields
you did not edit in Settings; explicitly edited fields take precedence.

Settings include:

- Provider account, model, and API key. Each provider retains its own values.
- Reasoning effort filtered to the selected model's capabilities.
- Agentic technique.
- Theme.
- Maximum tool iterations.
- Ralph Loop iterations.
- Maximum ReAct passes per plugin turn.
- Temperature.
- Options declared by installed plugins.
- Built-in editors for `tools.md` and `skills.md`.

The API key field starts hidden and has a **Show key** radio control plus
**Copy**, **Paste**, and **Clear** buttons. Focus **Show key** with Tab or arrows
and press `Enter` to toggle between `( )` hidden and `(*)` visible.
Paste replaces the entire key and removes surrounding
whitespace, matching quotes, and an optional `Bearer ` prefix. Malformed pastes
leave the current key unchanged. Focus the field and press `i`/`a` to select it
for replacement, `Ctrl+A` to select all, `Ctrl+C` to copy, or `Ctrl+V` to paste.
Copy places the secret on the system clipboard; visibility
is reset when Settings closes or the provider changes. Clipboard buttons use
native macOS APIs or optional `wl-clipboard`/`xclip` utilities on Linux.

Settings uses the same modal navigation style:

| Key | Action |
| --- | --- |
| `j` / `k` or arrows | Change the focused list selection |
| `Tab` | Move between controls; cycles a focused list |
| `Enter` | Activate the focused control |
| `i` / `a` | Edit the focused text field |
| `Esc` | Return to Normal mode or close Settings |
| `q` | Close Settings from Normal mode |

When editing `tools.md` or `skills.md`, use the normal editor commands plus
`s` to save and `q` to cancel. Saved file changes are loaded on the next agent
turn.

## Agentic Techniques

| Technique | Behavior | Suggested use |
| --- | --- | --- |
| Standard ReAct | Lets the model alternate between reasoning, tools, and a final answer | Most coding tasks |
| Chain of Thought | Adds a step-by-step reasoning preamble | Multi-step diagnosis and planning |
| Ralph Loop | Repeats the goal for a configured number of improvement passes | Iterative refactors |
| RAG | Searches the workspace for query keywords and prepends matching excerpts | Large or unfamiliar projects |
| Deliberate [plugin] | Adds explicit hypotheses and verification before acting | Plugin demonstration and verification-focused work |

Press `t` in Normal mode to cycle techniques quickly, or choose one in
Settings. Deliberate is installed and active by default for fresh
configurations; existing users keep their saved technique.

## Technique Plugins

Technique plugins are native C++ dynamic libraries loaded when OVI-CODE starts.
They add entries to the setup wizard, Settings, and the `t` technique cycle
without recompiling or relinking the application.

> **Warning:** A plugin is native code with the same permissions as OVI-CODE.
> It is not sandboxed and can read files, access the network, or execute other
> processes. Install only plugins whose source and publisher you trust.

### Discovering Plugins

Without an override, OVI-CODE loads `.dylib` and `.so` modules from the
`plugins` directory beside the executable, the CMake prefix location
`lib/ovi-code/plugins`, and the user directory `~/.ovi-code/plugins`. The first
two locations contain the bundled Deliberate example in source builds,
portable archives, and prefix installations.

Use only another directory, replacing the default search list, with either
form:

```bash
ovi-code --plugin-dir /path/to/plugins
OVI_CODE_PLUGIN_DIR=/path/to/plugins ovi-code
```

List loaded modules and loader errors without opening the TUI:

```bash
ovi-code plugins
```

Plugins are discovered once at process startup. Restart OVI-CODE after adding
or rebuilding a module. A configured plugin that is missing or fails to load is
reported as an error and is never silently replaced by a built-in technique.

### Building The Example

The source tree includes `plugins/examples/deliberate.cpp`. It is built,
discovered, and installed by default with the application:

```bash
cmake -S . -B build
cmake --build build -j
./build/ovi-code plugins
./build/ovi-code
```

Fresh configurations select **Deliberate [plugin]** automatically. Its
**Verification depth** and **Show start message** options appear under
**Settings → Plugins**. Configure with `-DOVI_CODE_BUILD_EXAMPLE_PLUGINS=OFF`
to omit it.

### Writing A Plugin

Plugins include the public, dependency-free ABI header:

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
};

const OviCodeTechniquePluginV2 plugin{
    OVI_CODE_TECHNIQUE_PLUGIN_API_VERSION_V2,
    "verify-first",
    "Verify First",
    "Checks assumptions before acting",
    parameters,
    1,
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

Build it on macOS:

```bash
c++ -std=c++20 -dynamiclib -I/path/to/ovi-code/include \
  verify_first.cpp -o ~/.ovi-code/plugins/verify-first.dylib
```

Build it on Linux:

```bash
c++ -std=c++20 -shared -fPIC -I/path/to/ovi-code/include \
  verify_first.cpp -o ~/.ovi-code/plugins/verify-first.so
```

Create `~/.ovi-code/plugins` first if it does not exist. Plugin IDs must start
with a lowercase letter or digit and contain only lowercase letters, digits,
`.`, `_`, or `-`. They must not duplicate built-in or previously loaded IDs.

### Runtime Interface

The exported `ovi_code_get_technique_plugin_v2` function returns one
`OviCodeTechniquePluginV2` descriptor. V2 descriptors can declare string,
secret, integer, number, boolean, and choice settings. OVI-CODE renders them in
**Settings → Plugins**, persists values per plugin, and exposes the effective
value through `host->get_parameter`. V1 plugins remain loadable but cannot
declare settings.

| Field or callback | Purpose |
| --- | --- |
| `id`, `label`, `description` | Metadata shown and persisted by OVI-CODE |
| `create` / `destroy` | Optional process-lifetime plugin state |
| `run_turn` | Entry point for each user turn |
| `host->workspace` | Absolute active workspace path |
| `host->base_system_prompt` | Standard prompt plus project instruction files |
| `host->run_react` | Add a user message and run one complete model/tool pass |
| `host->last_assistant_text` | Inspect the latest response for loop stop conditions |
| `host->emit` | Write System or Error status to the transcript |
| `host->iteration_limit` | Maximum host-enforced ReAct passes for this turn |
| `host->get_parameter` | Read a V2 setting or its declared default |

A plugin can call `run_react` more than once to implement refinement, critic,
or Ralph-style loops. The host stops calls beyond the **Plugin passes** setting.
Callbacks and input pointers are valid only during `run_turn`; copy any data
that must be retained. Do not allow C++ exceptions to cross the exported ABI.

The current ABI version and exact declarations are in
[`include/ovicode/technique_plugin.h`](include/ovicode/technique_plugin.h).

## Curated Models And Effort

Each provider has its own curated model list. The OpenRouter picker includes
the latest Fable, Opus 5, and GPT-5.6 variants reported by OpenRouter:

| Family | Model IDs |
| --- | --- |
| Claude Fable | `anthropic/claude-fable-5`, `~anthropic/claude-fable-latest` |
| Claude Opus 5 | `anthropic/claude-opus-5`, `anthropic/claude-opus-5-fast` |
| GPT-5.6 Sol | `openai/gpt-5.6-sol`, `openai/gpt-5.6-sol-pro` |
| GPT-5.6 Terra | `openai/gpt-5.6-terra`, `openai/gpt-5.6-terra-pro` |
| GPT-5.6 Luna | `openai/gpt-5.6-luna`, `openai/gpt-5.6-luna-pro` |

Direct OpenAI, Anthropic, and xAI accounts use provider-native model IDs, while
OpenRouter IDs retain their vendor prefix. Reasoning effort is translated to
each provider's native API parameter. The available choices change with the
selected model:

- Fable and Opus 5 expose Max, Extra high, High, Medium, and Low.
- GPT-5.6 variants expose Max, Extra high, High, Medium, Low, and None.
- Other curated models expose only the levels listed by OpenRouter.
- Models without selectable reasoning and unknown custom IDs expose only Auto.

Auto follows model capabilities. It selects High for Fable and Opus 5, Medium
for GPT-5.6 and mandatory-reasoning models, and leaves optional reasoning
model-managed when OpenRouter does not enable it by default. If a saved effort
is not supported after changing models, OVI-CODE falls back to that model's
Auto behavior instead of sending an invalid value.

## Project Instructions

OVI-CODE reads three optional Markdown files from the workspace root. Use
`--agent-config-dir` to keep them in another directory.

| File | Purpose |
| --- | --- |
| `agent.md` | Project persona, conventions, constraints, and hard rules |
| `tools.md` | Additional instructions for how the model should use tools |
| `skills.md` | Reusable recipes and project-specific capabilities |

Example:

```bash
ovi-code \
  --workspace ~/Projects/my-project \
  --agent-config-dir ~/Projects/my-project/.ovi-code
```

These files are reloaded at the beginning of every agent turn. The repository
includes examples of all three files.

## Command Reference

```text
ovi-code [command] [options]
```

### Commands

| Command | Description |
| --- | --- |
| `init` | Run setup, save the configuration, and exit |
| `plugins` | List loaded runtime technique plugins and loader errors |
| `sessions` | List saved sessions for the selected workspace |
| `uninstall` | Remove the saved configuration and exit |

### Options

| Option | Description |
| --- | --- |
| `--workspace <dir>` | Set the workspace root; defaults to the current directory |
| `--agent-config-dir <dir>` | Set the directory containing `agent.md`, `tools.md`, and `skills.md` |
| `--config <file>` | Use a custom application configuration path |
| `--plugin-dir <dir>` | Replace default plugin discovery with one directory |
| `--setup` | Run the setup wizard again |
| `--init` | Alias for the `init` command |
| `--list-plugins` | Alias for the `plugins` command |
| `--uninstall` | Alias for the `uninstall` command |
| `--no-splash` | Skip the startup splash screen |
| `--new-session` | Start a new conversation while keeping earlier sessions |
| `--resume <id>` | Resume a specific saved session; default is the latest |
| `-h`, `--help` | Display command-line help |

### Environment

| Variable | Description |
| --- | --- |
| `OPENROUTER_API_KEY` | Override the saved OpenRouter API key |
| `OPENAI_API_KEY` | Override the saved OpenAI API key |
| `ANTHROPIC_API_KEY` | Override the saved Anthropic API key |
| `XAI_API_KEY` | Override the saved xAI/Grok API key |
| `OVI_CODE_PLUGIN_DIR` | Override the runtime technique plugin directory |
| `COLORFGBG` | Help automatic theme detection determine terminal brightness |

## Tools And Safety

The model can request three built-in tools:

| Tool | Behavior |
| --- | --- |
| `read_file` | Read a file relative to the workspace |
| `list_files` | Recursively list workspace files while skipping generated and Git directories |
| `edit_file` | Replace one unique text occurrence or create a new file |

Every tool path is resolved through the workspace sandbox. The transcript is
also bounded to prevent an indefinitely growing session from consuming memory.

An agent can still make broad changes inside the selected workspace. Use a
clean Git working tree, review diffs, and avoid opening directories containing
unrelated sensitive files.

## Configuration And Privacy

Application settings are stored in `~/.ovi-code/config.json`. The file includes
the saved key and model for each provider account and is restricted to the
current user with mode `0600`. Environment-provided keys are not persisted. Use
`--config` to choose another location.

Sessions are saved locally under `~/.ovi-code/sessions/`, grouped by canonical
workspace path. With `--config`, the `sessions/` directory is beside the selected
config file. Directories use mode `0700`; checkpoint and draft files use `0600`.
Saves are atomic and each session permits only one active writer.

Session files are **not encrypted**: prompts, responses, provider reasoning
state, and tool results can contain sensitive code or secrets. Provider account
keys are not copied into session metadata. Visible transcript limits and model
context compaction still apply; this is recovery storage, not an unlimited archive.
The `c` command clears the saved visible transcript, not model context. Use
`--new-session` to start with empty context. Neither that option nor `uninstall`
deletes earlier sessions; remove saved session files explicitly if desired.

Requests, assembled project instructions, and file contents returned by tools
are sent to the selected provider. OpenRouter requests may also be handled by
the model provider it routes to. Review the applicable privacy and retention
policies before using OVI-CODE with confidential code.

## Troubleshooting

### Provider authentication fails

Confirm Settings has the key for the selected provider, or set its environment
variable. OpenRouter keys must begin with `sk-or-v1-`. To reconfigure, run:

```bash
ovi-code --setup
```

### The application has the wrong project open

Quit and restart with an explicit workspace:

```bash
ovi-code --workspace /absolute/path/to/project
```

### The output does not scroll

Press `Esc` to enter Normal mode, press `Ctrl+W` to focus **output**, then use
`j`, `k`, `g`, or `G`.

### The terminal appears frozen

Some terminals use `Ctrl+S` and `Ctrl+Q` for XON/XOFF flow control. OVI-CODE
does not bind those shortcuts, but pressing `Ctrl+S` accidentally can still
pause terminal output. Press `Ctrl+Q` to resume it.

### Colors have poor contrast

Open Settings and choose Dark, Light, or Plain instead of automatic detection.
Plain uses a fixed high-contrast monochrome palette.

### Project instructions are not applied

Confirm the files are in the workspace root or pass their directory explicitly
with `--agent-config-dir`. Changes are loaded at the start of the next turn.

### A model request fails

Confirm the model ID exists for the selected provider, the account has credits,
and the provider API is reachable. Try another model from Settings if the
selected model does not support the requested operation.

## Uninstall

Remove the saved configuration:

```bash
ovi-code uninstall
```

Remove the Homebrew cask and its saved configuration:

```bash
brew uninstall --cask --zap ovi-code
```

For a source build, delete the cloned repository or its `build/` directory.

## Building From Source

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix ~/.local
```

The Makefile provides equivalent development commands:

```bash
make build
make run
make run ARGS="--workspace /path/to/project"
```

CMake downloads FTXUI and nlohmann/json during the first configure and links
against system libcurl. The installed executable is placed in the selected
prefix's `bin` directory.

On macOS, system libcurl is selected from the active SDK (or the SDK specified
with `CMAKE_OSX_SYSROOT`) so cached Command Line Tools paths do not conflict with
an Xcode linker. If a build reports a TAPI unknown-architecture error,
reconfigure it or follow the
[macOS build troubleshooting guide](https://ovicorps.github.io/ovi-code/troubleshooting/#macos-build-fails-with-a-tapi-unknown-architecture-error).

Build, preview, or deploy the documentation with:

```bash
make docs-install
make docs
make docs-serve
make docs-deploy DOCS_REF=main
```

`docs-deploy` requires an authenticated GitHub CLI and dispatches
`.github/workflows/docs.yml`, which publishes the selected ref to GitHub Pages.

## Homebrew Maintainers

Release automation builds checksummed Apple Silicon and Intel archives for
tagged versions. See
[`packaging/homebrew/README.md`](packaging/homebrew/README.md) for cask and tap
publishing instructions.
