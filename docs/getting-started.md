# Getting started

## Requirements

To run OVI-CODE you need an API key and sufficient credits for OpenRouter,
OpenAI, Anthropic, or xAI/Grok.

Building from source additionally requires:

- CMake 3.20 or newer
- A C++20 compiler
- System libcurl
- Git and an internet connection during the first CMake configure

## Install

=== "Homebrew"

    ```bash
    brew install --cask Ovicorps/ovi-code/ovi-code
    ```

=== "Build from source"

    ```bash
    git clone https://github.com/Ovicorps/ovi-code.git
    cd ovi-code
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ```

    The executable is `build/ovi-code`. Install it to another prefix with:

    ```bash
    cmake --install build --prefix ~/.local
    ```

    The install includes the Deliberate example technique plugin under the
    selected prefix. Build-tree runs discover the same plugin in
    `build/plugins` automatically.

## Configure

Run the application:

```bash
ovi-code
```

The first launch opens a five-step setup wizard:

1. Choose a provider account.
2. Enter that provider's API key.
3. Choose a curated model or enter a custom provider model ID.
4. Choose a reasoning effort supported by that model.
5. Choose the default agentic technique.

OVI-CODE includes the **Deliberate** example plugin and preselects it for a
fresh configuration. It demonstrates configurable plugins with options under
**Settings → Plugins**. Choose another technique during setup if preferred.

Configuration is stored at `~/.ovi-code/config.json` with permissions set to
`0600`. To rerun setup later:

```bash
ovi-code --setup
```

You can keep the key out of the configuration file by setting an environment
variable for the selected provider:

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
# Or: OPENAI_API_KEY, ANTHROPIC_API_KEY, XAI_API_KEY
ovi-code
```

## Open a workspace

Run OVI-CODE from the project directory:

```bash
cd ~/Projects/my-project
ovi-code
```

Or pass the workspace explicitly:

```bash
ovi-code --workspace ~/Projects/my-project
```

The workspace is the security boundary for built-in file tools. Tool paths that
resolve outside it are rejected.

## Send the first request

The prompt starts in Insert mode. Type a request, press ++esc++ to enter Normal
mode, and press ++enter++ to send it.

```text
Find the cause of the build failure, fix it, and rebuild the project.
```

During a turn, the selected model may answer directly or request file tools.
Tool activity is shown in the output panel and returned to the model until it
answers or reaches the configured iteration limit.

Conversation context stays in memory for the current process. Restart OVI-CODE
to begin a new conversation.

## Next steps

- Learn [keyboard navigation and Settings](user-guide.md).
- Compare the built-in [agentic techniques](techniques.md).
- Add project-specific instructions with `agent.md`, `tools.md`, and
  `skills.md`.
