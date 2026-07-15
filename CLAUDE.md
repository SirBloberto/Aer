# AER — Working Conventions

## Comments

Default to no comment. When one is genuinely needed, default to a single line. Reserve multi-line
comment blocks for cases with real, non-obvious complexity that truly can't be said in one line —
a hidden constraint, a subtle invariant, a workaround for a specific bug, or behavior that would
surprise a reader.

- Never restate what the code already says. Well-named identifiers make `i++; // increment i`
  always wrong.
- Never write a multi-line block just because a nearby function has one. Judge every comment on
  its own merits, not by matching the file's existing style.
- Never reference the current task, session, or a change's origin ("added for the X feature",
  "changed during the Y refactor", "fixed per the conversation about Z"). That belongs in the
  commit message, not the source — it rots the moment the codebase moves past that context.
- Before treating a change as done, re-check every comment in the diff against this policy. It
  does not stick automatically after a cleanup pass — each new diff needs the same check, or the
  density creeps back (it has, more than once).

This isn't a stylistic preference layered on top of the project — AER's own stated values are
light, fast, and easy to understand (see README), and a dense comment block works against "easy
to understand" exactly as much as an unnecessary abstraction does.

## Dead code

If a function, opcode, branch, or file is confirmed unused, delete it outright — don't comment it
out, don't leave a `// no longer used` marker, don't keep a re-export for compatibility. Verify
with a real search (callers, computed-goto dispatch tables, macro-generated references) before
deleting, since this codebase dispatches through function-pointer tables and packed opcodes that a
plain text search can miss.
