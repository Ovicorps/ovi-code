# Using the TUI

The chat screen contains an **output** panel for conversation and tool activity,
and an **input** panel with a multiline Vim-style prompt editor. The highlighted
panel title indicates keyboard focus.

## Saved sessions and recovery

Prompts, responses, tool activity, model context, and unfinished input are saved
automatically. Reopen OVI-CODE in the same workspace to continue the latest session,
even after the terminal or process was closed without a normal exit.

```bash
ovi-code --new-session         # Start fresh, keeping earlier sessions
ovi-code sessions              # List saved IDs for this workspace
ovi-code --resume <id>         # Resume a particular session
```

For an interrupted or failed turn, the submitted prompt is restored unless you
saved a newer draft. No request or tool is automatically rerun. Inspect existing
file changes before submitting again. When the current provider or model differs
from the saved session, only the transcript and draft are restored; model context
starts fresh. Settings changes that reset model context also reset its saved copy.

A session can be open in only one process. Use `--new-session` for a second chat
in the same workspace. Save failures display **NOT SAVED** and an error in the
output panel; submitting a new turn requires its input to be saved successfully.
An in-progress turn also stops if its output cannot be checkpointed.

## Insert mode

| Key | Action |
| --- | --- |
| Characters | Insert text |
| ++enter++ | Insert a newline |
| ++tab++ | Insert four spaces |
| Arrow keys | Move the cursor |
| ++home++ / ++end++ | Move to the start / end of the line |
| ++backspace++ / ++delete++ | Delete text |
| ++esc++ | Enter Normal mode |

## Normal mode

| Key | Action |
| --- | --- |
| ++h++ / ++j++ / ++k++ / ++l++ | Move left / down / up / right |
| ++w++ / ++b++ | Move forward / backward by word |
| ++0++ / `$` | Move to the start / end of the line |
| ++g++ ++g++ / ++shift+g++ | Move to the first / last line |
| ++i++ / ++a++ | Insert before / after the cursor |
| ++shift+i++ / ++shift+a++ | Insert at the start / end of the line |
| ++o++ / ++shift+o++ | Open a line below / above |
| ++x++ | Delete the character under the cursor |
| ++d++ ++d++ | Delete the current line |
| ++y++ ++y++ | Copy the current line |
| ++p++ / ++shift+p++ | Paste after / before the current line |
| ++enter++ | Send the prompt |
| ++ctrl+w++ | Switch between input and output |
| ++z++ | Minimize or restore the input panel |
| ++s++ | Open Settings |
| ++t++ | Cycle the selected technique |
| ++c++ | Clear the visible transcript |
| ++q++ | Quit |

The internal line clipboard is shared by the prompt, `tools.md`, and
`skills.md` editors.

## Output navigation

Press ++ctrl+w++ in Normal mode to focus the output panel.

| Key | Action |
| --- | --- |
| ++j++ / ++k++ | Scroll down / up |
| ++g++ / ++shift+g++ | Jump to the top / bottom |
| ++ctrl+w++ | Return to the input panel |

The application shortcuts ++s++, ++t++, ++c++, and ++q++ remain available
while output is focused.

## Settings

Press ++s++ from Normal mode. Settings are organized into five tabs:

| Tab | Contents |
| --- | --- |
| **Model** | Provider account, model, reasoning effort, and provider API key |
| **Agent** | Technique, execution limits, temperature, and instruction editors |
| **Appearance** | System, dark, light, and plain themes |
| **Plugins** | Options declared by installed technique plugins |
| **Shortcuts** | Chat and Settings keyboard reference |

The bundled **Deliberate** example exposes **Verification depth** and **Show
start message** in the Plugins tab. Fresh configurations use Deliberate by
default; change the active technique in the Agent tab or with ++t++.

Use ++tab++ to move between controls (or cycle a focused list) and ++j++ /
++k++ or arrow keys to change list selections. Press ++enter++ to activate a
focused button, or ++i++ / ++a++ before typing in a text field.

**Save changes** writes the configuration immediately and applies it to the
next turn. **Cancel**, ++q++, or ++esc++ from Normal mode closes Settings without
applying the form. A failed save leaves the form open and does not apply its
changes.

The active config file is reloaded before opening Settings and before each
turn, including files selected with `--config`. Updating a provider's API key
in the file therefore takes effect without restarting the app. If the file
changes while Settings is open, Save preserves fields you did not edit;
explicitly edited fields take precedence. Quitting does not overwrite a newer
file key with an unchanged in-memory key.

### Edit provider API keys

The Model tab provides a **Show key** radio control and **Copy**, **Paste**, and
**Clear** buttons. Focus **Show key** with Tab or arrows and press ++enter++ to
toggle it: `( )` keeps the key hidden and `(*)` shows it. Keys are hidden by
default and hidden again when you switch providers,
leave the Model tab, save, cancel, or reopen Settings. Visibility is never saved
in the configuration.

Focus the key and press ++i++ or ++a++ to select the entire value for replacement.
Use ++ctrl+a++ to select all, ++ctrl+c++ to copy, ++ctrl+v++ to replace from the
system clipboard, and ++ctrl+u++ to clear. Arrow
keys and Home/End let you edit individual characters. Enter finishes editing;
it does not save the form.

**Paste** and bracketed terminal paste replace the whole key rather than
appending to an old value. Surrounding whitespace, matching quotes, and an
optional `Bearer ` prefix are removed. Invalid multiline text, embedded control
characters, and oversized pastes are rejected without changing the current key.
Use **Save changes** to persist edits immediately; Cancel discards them.

Copy is explicit even while the key is hidden. Your system clipboard contains
the secret after copying, so avoid clipboard-history tools or sharing its
contents. On Linux, clipboard buttons require `wl-copy`/`wl-paste` or `xclip`;
terminal paste remains available without these tools.

## Project instructions

OVI-CODE reads three optional Markdown files from the workspace root:

| File | Purpose |
| --- | --- |
| `agent.md` | Project persona, conventions, constraints, and hard rules |
| `tools.md` | Additional guidance for using tools |
| `skills.md` | Reusable recipes and project-specific capabilities |

Use another directory when instructions should live outside the workspace:

```bash
ovi-code \
  --workspace ~/Projects/my-project \
  --agent-config-dir ~/Projects/my-project/.ovi-code
```

Instruction files are reloaded at the beginning of every turn. Settings
provides built-in editors for `tools.md` and `skills.md`; use ++s++ to save and
++q++ to cancel from those editors.

## Privacy and session data

- Provider keys are stored in `~/.ovi-code/config.json` unless supplied through
  `OPENROUTER_API_KEY`, `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, or `XAI_API_KEY`.
- Sessions are stored under `~/.ovi-code/sessions/`, or beside a custom `--config`
  file in `sessions/`. Directories use mode `0700`; files use `0600`.
- Session files are not encrypted and may include sensitive prompts, code, tool
  results, and provider reasoning state. Account API keys are not included in
  session metadata. Existing transcript/context limits still apply.
- Clearing the transcript with ++c++ does not clear model conversation context.
- Use `--new-session` for empty context. Earlier sessions remain saved; `uninstall`
  removes configuration, not session data. Delete saved session files explicitly
  to remove history.
- Requests, instructions, and file contents returned by tools are sent to the
  selected provider. OpenRouter may route them to a separate model provider.
