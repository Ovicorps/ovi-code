# OVI-CODE

A tiny code-editing TUI agent in C++ — inspired by Amp's *How to Build an
Agent*. ReAct-style tool loop, three file tools (`read_file`, `list_files`,
`edit_file`), an FTXUI terminal UI, and access to LLMs through OpenRouter.

## Features

- **OpenRouter**: one API key with a curated model picker and custom model IDs.
- **Setup wizard** on first run: paste an OpenRouter API key and pick a model.
- **TUI**: full-screen chat with live transcript of model output, tool calls,
  and tool results. Vim-style modal input (Insert by default; Esc → Normal)
  with single-key actions for settings, technique, clear, and quit.
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

## Install via Homebrew

```bash
brew install Ovicorps/ovi-code/ovi-code
```

This taps `Ovicorps/homebrew-ovi-code` and builds OVI-CODE from source
(CMake pulls in FTXUI + nlohmann/json automatically). Bleeding edge from
`main`: `brew install --HEAD Ovicorps/ovi-code/ovi-code`.

Maintainers: see [`packaging/homebrew/README.md`](packaging/homebrew/README.md)
for how to cut a tagged release and publish the tap.

## Running

```bash
./build/ovi-code                       # use cwd as workspace
./build/ovi-code --workspace ~/proj
./build/ovi-code --agent-config-dir ~/proj/.ovi-code
./build/ovi-code --setup               # re-run the setup wizard
./build/ovi-code init                  # configure, save, and exit
./build/ovi-code uninstall             # remove saved configuration
```

The first launch opens a 3-step setup wizard: OpenRouter API key → model →
default technique. Settings persist to
`~/.ovi-code/config.json` (mode 0600).

`ovi-code uninstall` removes only the saved configuration. If installed with
Homebrew, remove the binary separately with `brew uninstall ovi-code`.

You can override the saved key at runtime with `OPENROUTER_API_KEY`.

## Keybindings (chat screen)

The chat screen is modal, vim-style. You start in **Insert** mode (typing
fills the prompt). Hit `Esc` to enter **Normal** mode where single letters
drive actions. No Ctrl-key shortcuts — those collide with terminal
XON/XOFF flow control on many setups.

### Insert mode

| Key     | Action                          |
| ------- | ------------------------------- |
| `Enter` | Send the prompt                 |
| `Esc`   | Switch to Normal mode           |

### Normal mode

| Key                  | Action                    |
| -------------------- | ------------------------- |
| `i` / `a` / `Enter`  | Back to Insert mode       |
| `q`                  | Quit                      |
| `s`                  | Open settings             |
| `t`                  | Cycle agentic technique   |
| `c`                  | Clear the transcript view |
| `yy`                 | Copy the current line     |
| `p` / `P`            | Paste after / before      |

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

Open Settings with `s`, then choose **Edit tools.md** or **Edit skills.md** to
edit either file in the built-in Vim-style editor. In Normal mode, use `s` to
save or `q` to cancel. Files are read from and written to `--agent-config-dir`
(the workspace by default), and saved changes apply on the next agent turn.

This repository ships example versions of all three at the root.

## A Poem About a Duck

```
A little duck waddles along the shore,
with orange feet and nothing more
than a beak full of bread and a heart full of song,
quacking softly all day long.

He dips beneath the glassy lake,
leaves a ripple in his wake,
then pops back up without a care —
shaking droplets from his hair.

If you're debugging late at night
and nothing seems to work quite right,
just think about that duck, my friend:
he'll quack you through it in the end.
```

## A Poem About a Cat

```
A small gray cat on silent feet
slips like moonlight down the street,
with emerald eyes and whiskers bright,
keeper of the silver night.

She curls beside the window pane,
and listens to the gentle rain,
then purrs as if the world were made
for quiet dreams and striped shade.

If code begins to snarl and spat,
take a lesson from the cat:
stretch, blink slow, then try once more —
and peace will pad across the floor.
```
