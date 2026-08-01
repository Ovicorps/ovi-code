# OVI-CODE

OVI-CODE is a terminal-based coding agent written in C++20. It connects to
models through [OpenRouter](https://openrouter.ai/), inspects a selected
workspace, and can read, create, and edit files while showing every tool call
in a full-screen TUI.

## Contents

- [Quick Start](#quick-start)
- [Using The TUI](#using-the-tui)
- [Agentic Techniques](#agentic-techniques)
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
- OpenRouter model picker with support for custom model IDs.
- Visible model responses, tool calls, tool results, and file diffs.
- Workspace-sandboxed file tools.
- Standard ReAct, Chain of Thought, Ralph Loop, and RAG techniques.
- Project instructions through `agent.md`, `tools.md`, and `skills.md`.
- Dark, light, system, and high-contrast plain themes.
- Native Apple Silicon and Intel Homebrew cask packaging.

## Requirements

To use OVI-CODE, you need:

- macOS or another environment capable of building the C++ application.
- A terminal with UTF-8 and color support.
- An [OpenRouter API key](https://openrouter.ai/keys) beginning with
  `sk-or-v1-`.
- An OpenRouter account with sufficient credits for the selected model.

Building from source additionally requires CMake 3.20+, a C++20 compiler,
system libcurl, Git, and an internet connection during the first configure.

## Quick Start

### 1. Install

Install the prebuilt macOS cask after the Ovicorps tap has been published:

```bash
brew install --cask Ovicorps/ovi-code/ovi-code
```

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

The first launch opens a three-step setup wizard:

1. Enter your OpenRouter API key.
2. Select an OpenRouter model or enter a custom model ID.
3. Select the default agentic technique.

The configuration is saved to `~/.ovi-code/config.json` with permissions set
to `0600`.

You can also provide an API key through the environment:

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
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

The conversation remains in memory for the current session. Restart OVI-CODE
to begin a new conversation.

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

Press `s` from Normal mode to open Settings. Settings changes are applied to
the next agent turn and saved when the application exits.

Settings include:

- OpenRouter model and API key.
- Agentic technique.
- Theme.
- Maximum tool iterations.
- Ralph Loop iterations.
- Temperature.
- Built-in editors for `tools.md` and `skills.md`.

Settings uses the same modal navigation style:

| Key | Action |
| --- | --- |
| `j` / `k` / `h` / `l` | Navigate fields and choices |
| Arrow keys | Navigate fields and choices |
| `Tab` | Move between controls |
| `Space` / `Enter` | Select or activate the focused control |
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

Press `t` in Normal mode to cycle techniques quickly, or choose one in
Settings.

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
| `uninstall` | Remove the saved configuration and exit |

### Options

| Option | Description |
| --- | --- |
| `--workspace <dir>` | Set the workspace root; defaults to the current directory |
| `--agent-config-dir <dir>` | Set the directory containing `agent.md`, `tools.md`, and `skills.md` |
| `--config <file>` | Use a custom application configuration path |
| `--setup` | Run the setup wizard again |
| `--init` | Alias for the `init` command |
| `--uninstall` | Alias for the `uninstall` command |
| `--no-splash` | Skip the startup splash screen |
| `-h`, `--help` | Display command-line help |

### Environment

| Variable | Description |
| --- | --- |
| `OPENROUTER_API_KEY` | Override the saved OpenRouter API key |
| `NO_COLOR` | Disable colors in the startup splash |
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
the OpenRouter API key and is restricted to the current user with mode `0600`.
Use `--config` to choose another location.

Chat history is not persisted to disk. The `c` command clears only the visible
transcript; it does not remove the current conversation context sent to the
model. Restart the application to begin with empty context.

Requests, assembled project instructions, and file contents returned by tools
may be sent to OpenRouter and the selected model provider. Review OpenRouter's
privacy and retention policies before using OVI-CODE with confidential code.

## Troubleshooting

### OpenRouter authentication fails

Use an OpenRouter key beginning with `sk-or-v1-`, not a provider-native OpenAI,
Anthropic, or Google key. Update it in Settings or run:

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

Confirm the model ID exists on OpenRouter, the account has credits, and the
network can reach `https://openrouter.ai`. Try another model from Settings if
the selected model does not support the requested operation.

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

CMake downloads FTXUI and nlohmann/json during the first configure and links
against system libcurl. The installed executable is placed in the selected
prefix's `bin` directory.

## Homebrew Maintainers

Release automation builds checksummed Apple Silicon and Intel archives for
tagged versions. See
[`packaging/homebrew/README.md`](packaging/homebrew/README.md) for cask and tap
publishing instructions.
