# CLI reference

```text
ovi-code [command] [options]
```

## Commands

| Command | Description |
| --- | --- |
| `init` | Run setup, save configuration, and exit |
| `plugins` | List runtime technique plugins and loader errors |
| `sessions` | List saved sessions for the selected workspace, newest first |
| `uninstall` | Remove the saved configuration and exit |

## Options

| Option | Description |
| --- | --- |
| `--workspace <dir>` | Workspace root; defaults to the current directory |
| `--agent-config-dir <dir>` | Directory containing `agent.md`, `tools.md`, and `skills.md` |
| `--config <file>` | Custom application configuration path |
| `--plugin-dir <dir>` | Runtime technique plugin directory |
| `--setup` | Run the setup wizard again |
| `--init` | Alias for `init` |
| `--list-plugins` | Alias for `plugins` |
| `--uninstall` | Alias for `uninstall` |
| `--no-splash` | Skip the startup splash |
| `--new-session` | Start fresh while retaining previous saved sessions |
| `--resume <id>` | Resume a specific workspace session; default is the latest |
| `-h`, `--help` | Print CLI and keyboard help |

## Environment variables

| Variable | Description |
| --- | --- |
| `OPENROUTER_API_KEY` | Override the API key saved in configuration |
| `OPENAI_API_KEY` | Override the saved OpenAI API key |
| `ANTHROPIC_API_KEY` | Override the saved Anthropic API key |
| `XAI_API_KEY` | Override the saved xAI/Grok API key |
| `OVI_CODE_PLUGIN_DIR` | Override the runtime plugin directory |
| `COLORFGBG` | Help automatic theme detection determine terminal brightness |

## Files and directories

| Path | Purpose |
| --- | --- |
| `~/.ovi-code/config.json` | Saved settings and provider accounts; mode `0600` |
| `~/.ovi-code/sessions/` | Workspace-grouped recovery checkpoints and drafts; files `0600`, directories `0700` |
| `~/.ovi-code/plugins/` | Default native technique plugin directory |
| `<workspace>/agent.md` | Project persona, conventions, and constraints |
| `<workspace>/tools.md` | Project-specific tool instructions |
| `<workspace>/skills.md` | Reusable project recipes and capabilities |

Use `--config`, `--plugin-dir`, or `--agent-config-dir` to override the relevant
default path.
With `--config`, session storage is in `sessions/` beside the selected config file.
`sessions` does not require an API key. `--new-session` and `--resume` are mutually
exclusive. Interrupted tools are never automatically replayed.

## Built-in tools

| Tool | Behavior |
| --- | --- |
| `read_file` | Read a file relative to the workspace |
| `list_files` | Recursively list workspace files while skipping generated and Git directories |
| `edit_file` | Replace one unique occurrence or create a file |

All tool paths pass through the workspace path-traversal guard. This protects
files outside the workspace, but the agent can still make broad changes inside
it.

## Build commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix ~/.local
```

CMake fetches FTXUI and nlohmann/json during the first configure and links
against system libcurl.
