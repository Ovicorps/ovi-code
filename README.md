# ovi-code

A tiny code-editing TUI agent in C++ — inspired by Amp's *How to Build an
Agent*. ReAct-style tool loop, three file tools (`read_file`, `list_files`,
`edit_file`), an FTXUI terminal UI, and pluggable LLM providers.

## Features

- **Providers**: Anthropic, OpenAI, Google Gemini, Ollama (local).
- **Setup wizard** on first run: pick provider, paste API key, pick model.
- **TUI**: full-screen chat with live transcript of model output, tool calls,
  and tool results. In-app settings panel (Ctrl+S), technique switcher
  (Ctrl+T), clear (Ctrl+L), quit (Ctrl+Q).
- **Agentic techniques** (selectable at runtime):
  - **Standard** — plain ReAct.
  - **Chain of Thought** — adds a reasoning preamble.
  - **Ralph Loop** — re-issues the same goal N times until the model emits
    `DONE`. Useful for iterative refactors.
  - **RAG** — grep-based keyword retrieval over the workspace, prepended as
    context.
- **Workspace config** via `agent.md`, `tools.md`, `skills.md` in the
  workspace root (or in `--agent-config-dir`).

## Building

Requires CMake 3.20+, a C++20 compiler (clang or gcc), and an internet
connection on the first configure (CMake `FetchContent` downloads FTXUI, cpr,
and nlohmann/json).

```bash
cmake -S . -B build
cmake --build build -j
```

The binary lands at `build/ovi-code`.

## Running

```bash
./build/ovi-code                       # use cwd as workspace
./build/ovi-code --workspace ~/proj
./build/ovi-code --agent-config-dir ~/proj/.ovi-code
./build/ovi-code --setup               # re-run the setup wizard
```

The first launch opens a 4-step setup wizard: provider → API key →
model → default technique. Settings persist to
`~/.config/ovi-code/config.json` (mode 0600).

You can override the saved key at runtime with `OVI_CODE_API_KEY`.

## Keybindings (chat screen)

| Key       | Action                          |
| --------- | ------------------------------- |
| `Enter`   | Send the prompt                 |
| `Ctrl+S`  | Open settings                   |
| `Ctrl+T`  | Cycle agentic technique         |
| `Ctrl+L`  | Clear the transcript view       |
| `Ctrl+Q`  | Quit                            |

## Tools exposed to the model

| Tool         | Behaviour                                                              |
| ------------ | ---------------------------------------------------------------------- |
| `read_file`  | Read a file relative to the workspace.                                 |
| `list_files` | List a directory recursively (skips `.git/` and `build/`).             |
| `edit_file`  | Replace a unique `old_str` with `new_str`. Empty `old_str` creates.    |

Tool calls are sandboxed to within `--workspace`.

## Configuration files

If present in the workspace (or `--agent-config-dir`), these files are
concatenated into the system prompt:

- `agent.md` — persona, hard rules, style.
- `tools.md` — notes appended to each tool's usage.
- `skills.md` — named recipes the model can reference.

This repository ships example versions of all three at the root.
