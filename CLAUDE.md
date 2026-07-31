# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

```bash
cmake -S . -B build         # first configure fetches FTXUI + nlohmann/json (~30s)
cmake --build build -j      # incremental rebuilds; binary at build/ovi-code
./build/ovi-code             # launches TUI; first run drops into the 3-step setup wizard
./build/ovi-code --setup     # force re-run the setup wizard
./build/ovi-code --workspace <dir> --agent-config-dir <dir>
OPENROUTER_API_KEY=... ./build/ovi-code  # env override for the saved key
```

There are **no tests** and **no linter wired up**. CMake compiles with `-Wall -Wextra -Wpedantic`; warnings are treated as the only signal.

Config persists to `~/.ovi-code/config.json` (mode 0600). Delete it to re-trigger the wizard.

## Architecture

Single-process C++20 binary. Layering, bottom-up:

- **`http.{hpp,cpp}`** — minimal libcurl POST wrapper (`ovicode::http::post(url, headers, body)`). Used by the OpenRouter client. Originally used `cpr`, but cpr's libcurl build hung in a CMake try_compile on macOS; replaced with system libcurl via `find_package(CURL)`.
- **`tools.{hpp,cpp}`** — three workspace-sandboxed tools (`read_file`, `list_files`, `edit_file`) and their JSON Schemas. `edit_file` enforces unique-occurrence matching; empty `old_str` means "create new file". `resolveInside()` is the only path-traversal guard — every tool must route through it.
- **`providers.{hpp,cpp}`** — OpenRouter chat-completions client. The internal `ChatRequest` / `ChatResponse` / `Message` types remain wire-format agnostic; this layer translates them to OpenAI-compatible messages and tool calls for OpenRouter.
- **`agent_md.{hpp,cpp}`** — loads `agent.md` / `tools.md` / `skills.md` from the workspace (or `--agent-config-dir`) and assembles the system prompt. Technique-specific prefaces are layered in here.
- **`react.{hpp,cpp}`** — the ReAct loop. `runTurn()` is the entry point; it handles all four techniques:
  - `Standard` — straight ReAct loop capped by `max_tool_iterations`.
  - `ChainOfThought` — adds a "think step by step" preface.
  - `RalphLoop` — wraps the ReAct loop and re-injects the goal up to `ralph_iterations` times; stops early when the assistant emits `DONE`.
  - `Rag` — runs a keyword grep over the workspace and prepends matches as a synthetic user message *before* the real user turn.
- **`tui.{hpp,cpp}`** — FTXUI. Two screens: `runSetupWizard` (3 steps, its own `TerminalOutput` screen) and `runChatScreen`. Settings are no longer a separate screen — `makeSettingsPanel` builds them as an embeddable component that `runChatScreen` layers over the chat with `ftxui::Modal`.
- **`config.{hpp,cpp}`** — `Config` struct, technique/theme enums, JSON load/save, and the curated OpenRouter model list.
- **`main.cpp`** — arg parsing + setup-or-chat dispatch.

The `Config` struct is mutated live by the settings screen and re-saved on quit. `AgentContext` (config + loaded `agent.md` + workspace path) is rebuilt at the start of every turn so workspace `.md` edits and settings changes take effect immediately.

## Things to know before editing

- **The chat screen is modal (vim-style).** Insert mode types into the prompt; `Esc` switches to Normal mode where single-letter keys dispatch actions (`q`/`s`/`t`/`c`, `i`/`a`/`Enter` back to Insert). The mode lives in a local `ChatMode` enum in `runChatScreen` and is gated by the `CatchEvent` wrapper around the `Input` component — in Normal mode every key is swallowed so the input never receives characters. Do **not** reintroduce Ctrl+S / Ctrl+Q bindings: those collide with terminal XON/XOFF flow control and silently freeze the TUI on many setups. FTXUI v5 has no `Event::CtrlX` named constants anyway — if you ever need Ctrl+letter, build it via `Event::Special({N})` (A=1 … Z=26).
- **Settings are an in-screen modal, not a second `ScreenInteractive`.** `runChatScreen` builds the chat renderer and `makeSettingsPanel(...)`'s component, then wraps them with `auto app = Modal(chat, settings.component, &settings_open); screen.Loop(app);`. Opening settings just sets `settings_open = true` (after `settings.sync()` re-reads `cfg`); Save/Cancel call the `close` callback which sets it back to false. FTXUI's `Modal` routes events to whichever layer is active via an internal `Container::Tab`, so there is exactly one live screen — **do not** spin up a nested `ScreenInteractive` from an event handler (FTXUI's lifecycle assumes one active screen), and do not reintroduce the old `while`/`Next::OpenSettings` teardown loop (it caused an alternate-buffer swap flash on every settings open/close). The panel's mutable state lives in a `shared_ptr<SettingsState>` so the pointers bound into its Radiobox/Input components stay valid for the chat screen's whole lifetime.
- **The chat worker thread is joined, not detached.** Holding a `std::thread worker;` member lets `runChatScreen` set `stop_requested` and `join()` before returning. The `TranscriptSink` callback returns `!stop_requested.load()` so an aborted turn unwinds cleanly through the ReAct loop. Keep this invariant — detaching will reintroduce a use-after-free risk on quit.
- **Don't add raw `Color::*` constants in `tui.cpp`.** All chrome colors live in the `Palette` struct near the top of `tui.cpp` (dark + light variants) and are resolved per-render via `palette(cfg.theme)`. Auto-detection reads `$COLORFGBG`. New UI elements should pick a semantic slot (`pal.accent`, `pal.muted`, `pal.error`, etc.) rather than hard-coding a color — otherwise the light theme breaks. Transcript entry colors come from `kindColor(pal, kind)`.
- **Transcript is bounded.** `runChatScreen` keeps at most ~1000 entries (`kTranscriptHardCap`); when it overflows it drops the oldest in one batch and inserts an "elided" system note. All push sites go through `pushEntryLocked` (caller holds `mu`). If you add a new code path that writes to the transcript, use the helper — direct `transcript.push_back` will silently bypass the cap.
- **OpenRouter is the only provider.** Add curated model IDs to `knownModels()` in `config.cpp`; users can enter any other OpenRouter model ID through the custom model field.
- **Adding a new technique** means: add to `Technique` enum + `technique*` helpers + `allTechniques()` in `config.cpp`, add a preamble case in `assembleSystemPrompt()` in `agent_md.cpp`, and (if the technique alters control flow like RalphLoop/Rag) branch on it in `runTurn()` in `react.cpp`. The setup wizard and settings screen pick it up automatically via `allTechniques()` / `techniqueLabel()`.
- **`builtinTools()` is the only tool registry.** Anything new needs a `ToolDefinition` there plus a handler branch in `executeTool()`. OpenRouter picks it up automatically because requests serialize from the same list.

## Why no cpr

The first build hung for 30+ minutes on a runaway `cmTC_*` try-compile inside cpr's bundled libcurl build. macOS ships libcurl in the SDK (`/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib/libcurl.tbd`); `http.cpp` is ~60 lines of `curl_easy_*` — adding a vendored HTTP library back in costs more than it saves.
