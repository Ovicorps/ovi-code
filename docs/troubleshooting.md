# Troubleshooting

## Provider authentication fails

Confirm the **Model** Settings tab contains the selected provider's key, or set
`OPENROUTER_API_KEY`, `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, or `XAI_API_KEY`.
OpenRouter keys must begin with `sk-or-v1-`. To rerun setup:

```bash
ovi-code --setup
```

## The wrong project is open

Quit and restart with an explicit workspace:

```bash
ovi-code --workspace /absolute/path/to/project
```

## Output does not scroll

Press ++esc++ to enter Normal mode, ++ctrl+w++ to focus **output**, then use
++j++, ++k++, ++g++, or ++shift+g++.

## The terminal appears frozen

Many terminals reserve ++ctrl+s++ and ++ctrl+q++ for XON/XOFF flow control.
OVI-CODE does not bind those keys, but accidentally pressing ++ctrl+s++ can
pause terminal output. Press ++ctrl+q++ to resume it.

## Colors have poor contrast

Open the **Appearance** Settings tab and choose Dark, Light, or Plain instead of
automatic detection. Plain uses a fixed, high-contrast monochrome palette.

## Project instructions are ignored

Confirm `agent.md`, `tools.md`, and `skills.md` are in the workspace root, or
pass their directory explicitly:

```bash
ovi-code --agent-config-dir /path/to/instructions
```

Changes are loaded at the start of the next turn.

## A model request fails

Confirm that:

- The model ID exists for the selected provider.
- The selected provider account has sufficient credits.
- The selected provider API is reachable.
- The selected model supports the requested operation.

Try a curated model from the **Model** Settings tab to rule out an invalid
custom model ID.

## A plugin does not appear

Run the plugin listing command with the same directory used by the TUI:

```bash
ovi-code plugins --plugin-dir /path/to/plugins
```

Check the reported loader errors, platform extension (`.dylib` on macOS or
`.so` on Linux), exported symbol, ABI version, and plugin ID. Restart OVI-CODE
after rebuilding the module.

## Reset configuration

Remove the saved configuration and run setup again:

```bash
ovi-code uninstall
ovi-code
```

For a Homebrew installation, remove both the application and saved data with:

```bash
brew uninstall --cask --zap ovi-code
```
