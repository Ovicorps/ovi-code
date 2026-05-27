# tools.md

Notes appended to the tool-usage section of the system prompt. Use it to
constrain or extend the built-in tools.

## read_file

- Always read a file before editing it. Never edit a file you have not read.
- Prefer reading the smallest path you need; if a file is enormous, look at
  related files first to find the right offset.

## list_files

- Use `list_files` once at the start of a fresh task to orient yourself.
- Skip directories like `build/` and `.git/` — they are already filtered, but
  do not waste turns recursing into generated output.

## edit_file

- `old_str` must occur exactly once. If it does not, widen it with surrounding
  context until it is unique — do not retry the same call.
- To create a new file, pass `old_str = ""` and the full contents as
  `new_str`.
- Make one logical change per `edit_file` call.
