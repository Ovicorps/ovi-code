# Troubleshooting

## Provider authentication fails

Confirm the **Model** Settings tab contains the selected provider's key, or set
`OPENROUTER_API_KEY`, `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, or `XAI_API_KEY`.
OpenRouter keys must begin with `sk-or-v1-`. To rerun setup:

```bash
ovi-code --setup
```

### OpenRouter 401: User not found

OpenRouter rejected the key used by the request. Create or verify your key at
[OpenRouter API Keys](https://openrouter.ai/settings/keys), then update it in
**Settings → Model** or the active config file. In the current format, the field
is `accounts.openrouter.api_key`, not a legacy top-level `api_key` or
`openrouter_api_key` field.

Settings saves immediately. File edits are reloaded before opening Settings or
starting the next turn. If you use `--config`, edit that file rather than the
default `~/.ovi-code/config.json`.

`OPENROUTER_API_KEY` still overrides the saved key. To use only the config key
for a run:

```bash
env -u OPENROUTER_API_KEY ovi-code
```

Never share your API key in logs or bug reports. A malformed or unreadable
config file blocks reloading; fix its JSON or permissions before retrying.

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

## macOS build fails with a TAPI unknown architecture error

An error mentioning `libcurl.tbd`, `malformed file`, or an unknown architecture
such as `arm64e.x1-macos` can mean the build is mixing an SDK from one Xcode or
Command Line Tools installation with another installation's linker. This is
not an error in the application's C++ source.

On macOS, CMake aligns system libcurl with the active SDK, or with the SDK
selected through `CMAKE_OSX_SYSROOT`. Reconfigure an existing build to refresh
stale SDK paths:

```bash
cmake -S . -B build
cmake --build build -j
```

If the error remains, check your active toolchain and SDK:

```bash
xcode-select -p
xcrun --find clang++
xcrun --find ld
xcrun --show-sdk-path
xcrun --show-sdk-version
```

Use a compatible, up-to-date Xcode or Command Line Tools installation. To test
the active toolchain in a new build directory without deleting existing work:

```bash
cmake -S . -B build-macos \
  -DCMAKE_CXX_COMPILER="$(xcrun --find clang++)" \
  -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"
cmake --build build-macos -j
```

Explicit custom curl installations remain supported. When using system curl,
select its SDK with `CMAKE_OSX_SYSROOT` rather than pointing curl at a different
SDK's `.tbd` file.

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
