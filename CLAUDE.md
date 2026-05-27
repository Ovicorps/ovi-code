# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

```bash
cmake -S . -B build         # first configure fetches FTXUI + nlohmann/json (~30s)
cmake --build build -j      # incremental rebuilds; binary at build/ovi-code
./build/ovi-code             # launches TUI; first run drops into the 4-step setup wizard
./build/ovi-code --setup     # force re-run the setup wizard
./build/ovi-code --workspace <dir> --agent-config-dir <dir>
OVI_CODE_API_KEY=... ./build/ovi-code  # env override for the saved key
```

There are **no tests** and **no linter wired up**. CMake compiles with `-Wall -Wextra -Wpedantic`; warnings are treated as the only signal.

Config persists to `~/.config/ovi-code/config.json` (mode 0600). Delete it to re-trigger the wizard.

## Architecture

Single-process C++20 binary. Layering, bottom-up:

- **`http.{hpp,cpp}`** — minimal libcurl POST wrapper (`ovicode::http::post(url, headers, body)`). Used by every provider. Originally used `cpr`, but cpr's libcurl build hung in a CMake try_compile on macOS; replaced with system libcurl via `find_package(CURL)`.
- **`tools.{hpp,cpp}`** — three workspace-sandboxed tools (`read_file`, `list_files`, `edit_file`) and their JSON Schemas. `edit_file` enforces unique-occurrence matching; empty `old_str` means "create new file". `resolveInside()` is the only path-traversal guard — every tool must route through it.
- **`providers.{hpp,cpp}`** — provider abstraction. The internal `ChatRequest` / `ChatResponse` / `Message` types are provider-agnostic. Each provider (`Anthropic`, `OpenAI`, `Ollama`, `Gemini`) translates to/from this normalized form. Gemini doesn't return tool-call IDs, so we synthesize them client-side. Ollama uses the OpenAI-compatible chat format with `/api/chat`.
- **`agent_md.{hpp,cpp}`** — loads `agent.md` / `tools.md` / `skills.md` from the workspace (or `--agent-config-dir`) and assembles the system prompt. Technique-specific prefaces are layered in here.
- **`react.{hpp,cpp}`** — the ReAct loop. `runTurn()` is the entry point; it handles all four techniques:
  - `Standard` — straight ReAct loop capped by `max_tool_iterations`.
  - `ChainOfThought` — adds a "think step by step" preface.
  - `RalphLoop` — wraps the ReAct loop and re-injects the goal up to `ralph_iterations` times; stops early when the assistant emits `DONE`.
  - `Rag` — runs a keyword grep over the workspace and prepends matches as a synthetic user message *before* the real user turn.
- **`tui.{hpp,cpp}`** — FTXUI. Three screens: `runSetupWizard` (4 steps), `runSettingsScreen`, `runChatScreen`.
- **`config.{hpp,cpp}`** — `Config` struct, provider/technique enums, JSON load/save, known-model lists per provider.
- **`main.cpp`** — arg parsing + setup-or-chat dispatch.

The `Config` struct is mutated live by the settings screen and re-saved on quit. `AgentContext` (config + loaded `agent.md` + workspace path) is rebuilt at the start of every turn so workspace `.md` edits and settings changes take effect immediately.

## Things to know before editing

- **FTXUI v5 has no `Event::CtrlS`/`CtrlL` etc.** — Ctrl+letter maps to the control character (`A=1 … Z=26`) wrapped in `Event::Special({N})`. The four shortcuts the app uses are defined as `kCtrlL`/`kCtrlQ`/`kCtrlS`/`kCtrlT` near the top of `tui.cpp`. Add new shortcuts the same way; don't reach for `Event::CtrlX`.
- **Don't open a second `ScreenInteractive` from inside an event handler.** `runChatScreen` uses an outer `while (true)` loop with a `Next` enum: settings open by setting `next = Next::OpenSettings`, calling `screen.ExitLoopClosure()()`, returning to the outer loop, calling `runSettingsScreen`, then re-entering chat. Don't replace this with an inline modal — FTXUI's lifecycle assumes one active screen.
- **The chat worker thread is joined, not detached.** Holding a `std::thread worker;` member lets `runChatScreen` set `stop_requested` and `join()` before returning. The `TranscriptSink` callback returns `!stop_requested.load()` so an aborted turn unwinds cleanly through the ReAct loop. Keep this invariant — detaching will reintroduce a use-after-free risk on quit.
- **Adding a new provider** means: add to `Provider` enum + `knownProviders()` + `knownModels()` + `provider*` string helpers in `config.cpp`, then add a `callX()` translator in `providers.cpp` and wire it into the `chat()` switch. Translators only need to convert the internal `Message` list to/from the provider's wire format.
- **Adding a new technique** means: add to `Technique` enum + `technique*` helpers + `allTechniques()` in `config.cpp`, add a preamble case in `assembleSystemPrompt()` in `agent_md.cpp`, and (if the technique alters control flow like RalphLoop/Rag) branch on it in `runTurn()` in `react.cpp`. The setup wizard and settings screen pick it up automatically via `allTechniques()` / `techniqueLabel()`.
- **`builtinTools()` is the only tool registry.** Anything new needs a `ToolDefinition` there plus a handler branch in `executeTool()`. All providers will pick it up automatically since they serialize from the same list.

## Why no cpr

The first build hung for 30+ minutes on a runaway `cmTC_*` try-compile inside cpr's bundled libcurl build. macOS ships libcurl in the SDK (`/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib/libcurl.tbd`); `http.cpp` is ~60 lines of `curl_easy_*` — adding a vendored HTTP library back in costs more than it saves.
