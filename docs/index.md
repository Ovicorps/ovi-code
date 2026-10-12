![OVI-CODE logo](assets/OviCode.png)

# OVI-CODE

**A fast, extendable and keyboard-driven coding agent for your terminal.**

OVI-CODE is a C++20 terminal application that connects to OpenRouter, OpenAI,
Anthropic, or xAI/Grok. It can inspect a workspace, read and edit files, and
show every model response, tool call, result, and diff in a full-screen
interface.

Built-in techniques are just the starting point. Native C++ plugins let you
customize how the agent approaches a task, runs model/tool passes, and verifies
its work without rebuilding OVI-CODE.

<div class="grid cards" markdown>

-   **Extend your workflow**

    ---

    Add your own agentic techniques through runtime-loaded plugins, with
    configurable options integrated into Settings.

-   **Work in the terminal**

    ---

    Use a Vim-style multiline editor and keep the conversation, tool activity,
    and file changes visible in one screen.

-   **Choose the right model**

    ---

    Select a provider account and curated model, enter any custom model ID, and
    use only the reasoning-effort levels supported by that model.

-   **Keep tools in the workspace**

    ---

    File tools resolve paths inside the selected workspace and reject path
    traversal outside it.

</div>

## Built for extensibility

The technique plugin system separates your agent's workflow from the
application itself. Write a native C++ module against a dependency-free,
versioned C ABI, install it, and select it alongside the built-in techniques.
There is no need to fork, recompile, or relink the OVI-CODE binary.

- **Define the workflow:** tailor prompts, inspect the latest assistant
  response, and orchestrate multiple model/tool passes for review, refinement,
  or verification.
- **Reuse the host:** run the existing ReAct loop with the configured provider
  and workspace tools, and send progress messages to the transcript.
- **Expose your own settings:** declare typed options that appear under
  **Settings → Plugins** and are saved per plugin.

The bundled **Deliberate** plugin is a working example, not a special built-in
technique. It adds hypothesis-driven verification and configurable behavior
through the same public interface available to your plugins.

[Explore the plugin system](plugins.md){ .md-button .md-button--primary }

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

- Runtime-loaded C++ technique plugins through a versioned C ABI, with
  plugin-defined settings and custom multi-pass workflows.
- Bundled configurable Deliberate plugin, active on fresh installations.
- Full-screen TUI built with FTXUI.
- Vim-style prompt and instruction-file editors.
- OpenRouter, OpenAI, Anthropic, and xAI/Grok API clients with
  capability-aware reasoning effort.
- Workspace-scoped `read_file`, `list_files`, and `edit_file` tools.
- Project instructions from `agent.md`, `tools.md`, and `skills.md`.
- Dark, light, system, and high-contrast plain themes.

!!! warning "Review agent changes"
    OVI-CODE can make broad changes inside the selected workspace. Use version
    control, review diffs, and avoid opening directories that contain unrelated
    sensitive files.

## License

OVI-CODE's original source code and documentation are licensed under the
[Apache License, Version 2.0](license.md). Copyright © 2026 Ovicorps.
