# Using the TUI

The chat screen contains an **output** panel for conversation and tool activity,
and an **input** panel with a multiline Vim-style prompt editor. The highlighted
panel title indicates keyboard focus.

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

**Save changes** applies the configuration to the next turn. **Cancel**, ++q++,
or ++esc++ from Normal mode closes Settings without applying the form.

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
- Chat history is not persisted to disk.
- Clearing the transcript with ++c++ does not clear model conversation context.
- Requests, instructions, and file contents returned by tools are sent to the
  selected provider. OpenRouter may route them to a separate model provider.
