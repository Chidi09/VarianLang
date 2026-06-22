const { LanguageClient } = require('vscode-languageclient/node');
const vscode = require('vscode');

let client;

function activate(context) {
    const config = vscode.workspace.getConfiguration('varian');
    const command = config.get('path') || 'vn';
    const workspacePath = vscode.workspace.workspaceFolders ? vscode.workspace.workspaceFolders[0].uri.fsPath : undefined;

    const serverOptions = {
        run: {
            command: command,
            args: ['lsp'],
            options: { cwd: workspacePath }
        },
        debug: {
            command: command,
            args: ['lsp'],
            options: { cwd: workspacePath }
        }
    };

    const clientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'varian' },
            { scheme: 'file', language: 'lumen' }
        ]
    };

    client = new LanguageClient(
        'varianLsp',
        'Varian Language Server',
        serverOptions,
        clientOptions
    );

    client.start();
}

function deactivate() {
    if (!client) {
        return undefined;
    }
    return client.stop();
}

module.exports = {
    activate,
    deactivate
};
