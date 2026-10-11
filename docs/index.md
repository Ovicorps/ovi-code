![OVI-CODE logo](assets/OviCode.png)

# OVI-CODE

**A fast, keyboard-driven coding agent for your terminal.**

OVI-CODE is a C++20 terminal application that connects to OpenRouter, OpenAI,
Anthropic, or xAI/Grok. It can inspect a workspace, read and edit files, and
show every model response, tool call, result, and diff in a full-screen
interface.

<div class="grid cards" markdown>

-   **Work in the terminal**

    ---

    Use a Vim-style multiline editor and keep the conversation, tool activity,
    and file changes visible in one screen.

-   **Choose the right model**

    ---

    Select a provider account and curated model, enter any custom model ID, and
    use only the reasoning-effort levels supported by that model.

-   **Control the workflow**

    ---

    Switch between Standard ReAct, Chain of Thought, Ralph Loop, RAG, and
    runtime-loaded technique plugins.

-   **Keep tools in the workspace**

    ---

    File tools resolve paths inside the selected workspace and reject path
    traversal outside it.

</div>

## Quick start

```bash
brew install --cask Ovicorps/ovi-code/ovi-code
cd /path/to/project
ovi-code
```

The first launch asks for a provider account, API key, model, reasoning effort,
and default technique. The installed **Deliberate** example plugin is
preselected for fresh configurations and can be customized under **Settings →
Plugins**. Type a request in Insert mode, press ++esc++ to enter Normal mode,
then press ++enter++ to send it.

[Get started](getting-started.md){ .md-button .md-button--primary }
[Learn the TUI](user-guide.md){ .md-button }

## Core capabilities

- Full-screen TUI built with FTXUI.
- Vim-style prompt and instruction-file editors.
- OpenRouter, OpenAI, Anthropic, and xAI/Grok API clients with
  capability-aware reasoning effort.
- Workspace-scoped `read_file`, `list_files`, and `edit_file` tools.
- Project instructions from `agent.md`, `tools.md`, and `skills.md`.
- Dark, light, system, and high-contrast plain themes.
- Native C++ technique plugins through a versioned C ABI.
- Bundled configurable Deliberate plugin, active on fresh installations.

!!! warning "Review agent changes"
    OVI-CODE can make broad changes inside the selected workspace. Use version
    control, review diffs, and avoid opening directories that contain unrelated
    sensitive files.
