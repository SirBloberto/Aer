# AER Language Support for VS Code

Syntax highlighting for `.aer` files — a TextMate grammar, not a language server. No autocomplete,
no diagnostics, no go-to-definition; just correct colors for keywords, variables, function calls,
strings (including interpolation and triple-quoted strings), numbers, and operators.

This extension isn't published to the Marketplace — it lives in this repo and gets installed
locally. Two ways to do that; try the first, fall back to the second if your VS Code build doesn't
have that command.

## Method 1: Install from Location (fastest, if your VS Code has it)

1. Open the Command Palette — `Ctrl+Shift+P` (`Cmd+Shift+P` on macOS).
2. Run **Extensions: Install from Location...**
3. Select this folder (`editors/vscode-aer` in the AER repo) — pick the *folder*, not a file
   inside it.
4. Reload the window when prompted.

If that command doesn't show up in your palette at all (some VS Code versions/builds don't have
it), use Method 2 instead — it always works.

## Method 2: Copy into the extensions folder (universal fallback)

Copy this whole `vscode-aer` folder into your VS Code extensions directory, then **fully restart**
VS Code (not just "Reload Window" — a freshly copied extension folder needs a real restart to be
scanned).

**Windows (PowerShell):**
```powershell
Copy-Item -Path "<path-to-repo>\editors\vscode-aer" `
          -Destination "$env:USERPROFILE\.vscode\extensions\local.aer-language-0.1.0" `
          -Recurse -Force
```

**macOS / Linux (bash):**
```bash
cp -r <path-to-repo>/editors/vscode-aer ~/.vscode/extensions/local.aer-language-0.1.0
```

Then close VS Code completely and reopen it.

## Verify it worked

1. Open any `.aer` file (e.g. `tests/example.aer`).
2. Look at the bottom-right of the status bar — it should say **AER**, not "Plain Text".
   - If it still says "Plain Text": click it, and pick "AER" from the language picker manually.
     That confirms the extension *is* installed — file-association just didn't auto-detect — and
     you can leave it at that, or file an issue if `.aer` files should be auto-detected and aren't.
3. From a terminal, `code --list-extensions` should list `local.aer-language`. If it's *not* there
   after a full restart, VS Code isn't scanning the folder you copied into — see Troubleshooting.

## Troubleshooting

- **"Extensions: Install from Location..." isn't in the Command Palette.** Not present in every
  VS Code version — use Method 2.
- **Copied the folder but nothing changed.** You likely only reloaded the window instead of fully
  quitting and reopening VS Code. Extensions dropped directly into the extensions folder (as
  opposed to installed through the UI) are only picked up on a real startup scan.
- **`code --list-extensions` doesn't show it even after a restart.** Your install may use a
  non-default extensions path (e.g. VS Code Insiders, a portable install, or a remote/WSL window
  which has its *own* separate extensions folder inside the remote environment). Run
  `code --extensions-dir` — if unset, VS Code will tell you the effective path via
  **Help > About**; confirm you copied into the directory backing whichever VS Code window you're
  actually using.
- **It loaded, but colors look wrong for something specific.** Grammar bugs are easy to introduce
  and easy to fix — open an issue (or just describe what looked wrong) with the exact line of AER
  code, since a screenshot alone doesn't tell us which TextMate scope misfired.

## What's covered

- Keywords: `if` `else` `for` `in` `as` `struct` `function` `return` `break` `continue` `null`
  `import` `defer`, plus `true`/`false`
- Core builtins highlighted distinctly when called: `print` `length` `append` `delete` `type`
  `assert` `panic`
- Function *calls* (`translate(...)`, `math.sqrt(...)`) and function/struct *declarations* get
  distinct scopes — a declaration's name is `entity.name.function`/`entity.name.type`, a call site
  is `entity.name.function.call`
- Plain variables (reads, writes, parameters, field names) are scoped as `variable.other.readwrite`
  — without this, identifiers render in the theme's raw default foreground with no color at all,
  which is the "variables look unhighlighted" gap this grammar fixes
- `as Type` casts/shape-checks, with `Type` scoped as `support.type`
- Strings: escape sequences (`\n \t \\ \" \{`), `{identifier}` interpolation, and triple-quoted
  (`"""..."""`) strings
- Numbers (integer/float), comments (`#`), and the full operator set from the language reference
  (arithmetic, comparison, bitwise, logical, pipe `|>`, range `..`, compound assignment)

## Updating

Grammar lives in `syntaxes/aer.tmLanguage.json`. Two things worth knowing before editing it:

- **Pattern order is load-bearing.** TextMate tries top-level patterns in array order and the
  first match at a given position wins. The generic `#keywords` rule must stay *after* the more
  specific `#struct-declaration`/`#function-declaration`/`#cast-type` rules (otherwise the generic
  rule wins the race and the specialized name-capturing rules never fire), and `#variables` — the
  catch-all plain-identifier rule — must stay *last* of all, since it matches any identifier and
  would otherwise swallow keywords, builtins, and declaration names before they get a chance.
- Keyword/operator/builtin behavior should stay in sync with the tables in the project's top-level
  `README.md` (`## Keywords`, `## Operators`, `## Built-in Functions`) if the language grammar
  changes.
