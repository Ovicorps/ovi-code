# TUI flicker / flashing investigation

Audit of `src/tui.cpp` (FTXUI v5.0.0) for the reported flashing, flickering,
and stray-cursor glitches. Below is what was found, the root cause, the fix,
and the things that were considered but deliberately left alone.

## Root cause: a stray, unmanaged hardware cursor

**Symptom.** A blinking cursor artifact appears in the chat screen (and, more
subtly, in the settings/wizard screens while navigating menus). This is the
same "white/flashing" cursor the custom `VimEditor` was introduced to avoid —
but the editor only replaced the *visible* cursor with its own `inverted` cell;
it never disposed of FTXUI's *hardware* cursor.

**Mechanism.** FTXUI decides the terminal's real cursor position and shape from
the DOM during each `ScreenInteractive::Draw`:

- A `focusCursor*` node (e.g. `focusCursorBlock`) calls `screen.SetCursor(...)`
  with an explicit shape.
- A plain `focus` / `select` node hides the cursor.
- If **no** such node renders, `Screen::cursor_` keeps its previous value, and
  `Draw()` still emits `\x1b[?25h` (show cursor) plus a shape escape **every
  frame** (`screen_interactive.cpp:807-816`).

The chat screen has no cursor node at all:

- The only FTXUI component in the tree is the hidden `sink` `Input`, and the
  `Renderer` lambda never calls `sink->Render()`, so `sink` never positions the
  cursor.
- `focusPositionRelative(...) | yframe` (used for scrolling) is **not** a focus
  node — its `Render` is the default `NodeDecorator::Render` and touches no
  cursor (`focus.cpp:31-56`).

So `Screen::cursor_` is left at its default-constructed state. Its shape field
is declared `Screen::Cursor::Shape shape;` with **no initializer**
(`screen.hpp:108`), i.e. indeterminate. Every frame FTXUI then shows a cursor of
whatever that indeterminate shape happens to be (often a *blinking* block/bar),
parked in a stale cell — the flashing.

The settings and wizard screens are a milder case: they use real `Input`s
wrapped in `blockCursorInput` (which applies `focusCursorBlock` only to the
*focused* input). When focus moves to a `Menu` / `Radiobox` / `Button`, no
cursor node renders, so the last input's block cursor lingers and blinks in a
stale position.

## Fix

Pin the hardware cursor to `Hidden` at the top of each `Renderer` lambda:

```cpp
screen.SetCursor(Screen::Cursor{0, 0, Screen::Cursor::Hidden});
```

Why this is correct and order-safe:

- The lambda runs first in `Draw` (`document = component->Render()`), so it sets
  the default. Any `focusCursor*` node runs later in the same draw
  (`Render(*this, document)`) and *overrides* it.
- **Chat screen:** no cursor node exists, so the cursor stays `Hidden` every
  frame. The `VimEditor`'s `inverted` cell is the only visible cursor. FTXUI now
  emits an idempotent `\x1b[?25l` (hide) per frame instead of "show + blinking
  shape" — strictly less terminal churn, zero blink.
- **Settings / wizard:** the cursor is hidden by default and re-shown as a
  steady block *only* on the focused `Input`. Navigating menus no longer leaves
  a blinking cursor stranded at the last field.

Applied in all three render paths in `src/tui.cpp`:
`runSetupWizard`, `runSettingsScreen`, and `runChatScreen`.

## Follow-up: settings moved to an in-screen modal (swap flash fixed)

Originally deferred here was the **alt-screen swap flash on settings
open/close**: the chat screen is `ScreenInteractive::Fullscreen()` (alternate
buffer) while the old `runSettingsScreen` used `TerminalOutput()` (main buffer).
Round-tripping chat → settings → chat tore down the chat screen and swapped
terminal buffers, flashing once per transition.

This is now resolved. `runSettingsScreen` was replaced by `makeSettingsPanel`,
which builds the same form as an **embeddable component**. `runChatScreen` layers
it over the chat with `ftxui::Modal(chat, settings.component, &settings_open)`:

- One `ScreenInteractive` for the whole session — no buffer swap, no flash.
- `Modal` renders the panel centered with `clear_under` (chat cleared beneath
  it) and routes events to whichever layer is active via an internal
  `Container::Tab`.
- The outer `while`/`Next::OpenSettings` teardown loop is gone; opening settings
  is now just `settings.sync(); settings_open = true;`.

Panel state lives in a `shared_ptr<SettingsState>` so the pointers bound into its
Radiobox/Input components stay valid for the chat screen's lifetime.

## Considered and intentionally left as-is
- **Full transcript re-render every frame.** `runChatScreen` rebuilds every
  visible `TranscriptEntry` on each draw. This is a per-frame cost, not tearing,
  and is already bounded by `kTranscriptHardCap` (~1000 entries). Left as-is.
- **`PostEvent(Event::Custom)` after loop exit.** The worker thread can post a
  redraw event during the narrow window between the user quitting and the worker
  being joined. This is a robustness concern (potential post to a torn-down
  task sender), not a visual glitch, so it is out of scope for this pass and
  noted here for follow-up.

## Verification

`cmake --build build -j` compiles cleanly with `-Wall -Wextra -Wpedantic` (no
new warnings). The change is render-only and does not alter event handling,
threading, or layout.
