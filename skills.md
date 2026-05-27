# skills.md

Reusable recipes. Reference these by name when planning a task.

## refactor-rename

1. Use `list_files` to find all candidate files.
2. Use `read_file` to confirm every callsite.
3. For each file, use `edit_file` once with a uniquely scoped `old_str`.
4. After all edits, summarize the renamed symbol and the files touched.

## add-feature

1. Read `agent.md` and any relevant module docs first.
2. Sketch the change in one paragraph before touching code.
3. Implement the smallest end-to-end version; do not add hypothetical
   abstractions.
4. Stop after the feature works; do not refactor neighbouring code.

## fix-bug

1. Reproduce the bug by reading the failing code path.
2. Identify the root cause in one sentence.
3. Apply the smallest fix that addresses the cause, not the symptom.
4. Confirm no other call sites rely on the buggy behaviour.
