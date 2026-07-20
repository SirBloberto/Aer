# vscode-aer

Syntax highlighting and language server support for [AER](../../README.md).

- **Syntax highlighting** (`syntaxes/aer.tmLanguage.json`, a declarative TextMate grammar) needs no
  npm install, no build step, no Node runtime. The same grammar file is reusable outside VS Code
  too: Monaco Editor (the editor component VS Code itself is built on, published standalone) loads
  the identical TextMate grammar format, so it's the one artifact for both an editor extension and
  a syntax-highlighted code sample on a website.
- **Diagnostics, go-to-definition, and completion** are powered by `aer-lsp` (`source/tools/aer_lsp.c`
  in the main repo — links the real compiler directly, so diagnostics are exactly what `aer` itself
  would say). This does need one `npm install` (for `vscode-languageclient`, the thin client-side
  wire-protocol library — see `src/extension.js`) and a separately-built binary.

## Setup

1. Build the language server from the repo root: `make lsp-tool` (produces `binary/aer-lsp`).
   Either put it on your `PATH`, or set `aer.languageServerPath` in VS Code settings to its full path.
2. From `tools/vscode-aer/`: `npm install` (pulls in `vscode-languageclient` — the only dependency).
3. Install the extension locally (below), or press `F5` to try it without installing.

## Install locally (development / personal use)

Copy or symlink this folder into VS Code's extensions directory, then reload VS Code.

**Windows (PowerShell):**
```powershell
New-Item -ItemType SymbolicLink -Path "$env:USERPROFILE\.vscode\extensions\vscode-aer" -Target "<repo path>\tools\vscode-aer"
```

**macOS/Linux:**
```sh
ln -s "$(pwd)/tools/vscode-aer" ~/.vscode/extensions/vscode-aer
```

Then reload VS Code (`Developer: Reload Window` from the command palette). Open any `.aer` file —
the language mode should read "AER" in the bottom-right status bar, and diagnostics/go-to-definition/
completion should be live if `aer-lsp` is reachable (see Setup above).

## Try it without installing

Open `tools/vscode-aer` as the workspace root in VS Code and press `F5` to launch an Extension
Development Host with the grammar and language server already active — no install step at all.
(Still needs steps 1-2 under Setup done once first.)

## Packaging (only needed to publish or share a `.vsix`)

```sh
npm install -g @vscode/vsce
vsce package
```
