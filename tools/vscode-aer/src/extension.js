// extension.js — thin VS Code client shim. Deliberately plain JavaScript, not TypeScript: this
// file has zero AER-language logic in it (no grammar, no parsing) — it just spawns the real
// language server (aer-lsp, source/tools/aer_lsp.c, links the actual compiler) as a subprocess
// and forwards stdio, using VS Code's own vscode-languageclient package to speak the wire
// protocol. No TypeScript means no build step — only `npm install` is needed, once, to pull in
// vscode-languageclient itself.

const { workspace } = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client;

function activate(context) {
    // aer.languageServerPath (settings.json) lets a user point at a differently-located or
    // differently-named binary; defaults to expecting "aer-lsp" on PATH.
    const config = workspace.getConfiguration("aer");
    const serverCommand = config.get("languageServerPath", "aer-lsp");

    const serverOptions = {
        command: serverCommand,
        transport: TransportKind.stdio,
    };

    const clientOptions = {
        documentSelector: [{ scheme: "file", language: "aer" }],
    };

    client = new LanguageClient("aer", "AER Language Server", serverOptions, clientOptions);
    context.subscriptions.push(client);
    client.start();
}

function deactivate() {
    return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
