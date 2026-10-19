// Pluma for VS Code: the language client and the debugger contribution.
//
// Both are thin: the engine's own binaries do the work. The language server
// is `cuervo-tools lsp <project>` (pluma/lsp.hpp with the engine prelude, so
// completion and hover know the engine API) and the debug adapter is
// `cuervo --dap`, which speaks DAP on stdin/stdout. Plain JavaScript on
// purpose: no build step between editing this file and running it.

'use strict';

const os = require('os');
const path = require('path');
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client = null;
let output = null;

function first_workspace_folder() {
    const folders = vscode.workspace.workspaceFolders;
    return folders && folders.length > 0 ? folders[0].uri.fsPath : undefined;
}

// Settings may name a build-tree binary relative to the workspace; VS Code
// doesn't expand variables in settings itself.
function expand(value, folder) {
    let out = value || '';
    if (folder) out = out.split('${workspaceFolder}').join(folder);
    if (out === '~' || out.startsWith('~/')) out = path.join(os.homedir(), out.slice(1));
    return out;
}

function settings() {
    return vscode.workspace.getConfiguration('pluma');
}

function project_dir() {
    const folder = first_workspace_folder();
    const configured = expand(settings().get('projectPath', ''), folder);
    return configured !== '' ? configured : folder;
}

async function start_client() {
    const folder = first_workspace_folder();
    const tools = expand(settings().get('toolsPath', 'cuervo-tools'), folder);
    const args = ['lsp'];
    const project = project_dir();
    if (project) args.push(project);
    if (!settings().get('checkStyle', true)) args.push('--no-check-style');

    // No `transport`: TransportKind.stdio makes the client append `--stdio`, which
    // `cuervo-tools lsp` rejects; an Executable without one still talks over stdio.
    const server = { command: tools, args: args, options: { cwd: folder } };
    const client_options = {
        documentSelector: [{ scheme: 'file', language: 'pluma' }],
        synchronize: { fileEvents: vscode.workspace.createFileSystemWatcher('**/*.plm') },
        outputChannel: output,
    };
    client = new LanguageClient('pluma', 'Pluma Language Server', server, client_options);
    try {
        await client.start();
    } catch (err) {
        client = null;
        const action = await vscode.window.showErrorMessage(
            `Pluma: couldn't start the language server '${tools}' (${err.message || err}). ` +
                'Build cuervo-tools and set "pluma.toolsPath" to it, or put it on PATH.',
            'Open Settings'
        );
        if (action === 'Open Settings') {
            vscode.commands.executeCommand('workbench.action.openSettings', 'pluma.toolsPath');
        }
    }
}

async function stop_client() {
    if (client) {
        const c = client;
        client = null;
        await c.stop();
    }
}

async function restart_client() {
    await stop_client();
    await start_client();
}

// Fills in a launch configuration when there is no launch.json (F5 on a
// .plm file) and defaults `project` to the workspace.
class PlumaConfigurationProvider {
    resolveDebugConfiguration(folder, config) {
        if (!config.type && !config.request && !config.name) {
            config.type = 'pluma';
            config.request = 'launch';
            config.name = 'Run game (Pluma)';
        }
        if (!config.project) {
            config.project = folder ? folder.uri.fsPath : project_dir();
        }
        if (!config.project) {
            return vscode.window.showErrorMessage('Pluma: open the game\'s project folder (the one with project.toml) to debug it.')
                .then(() => undefined);
        }
        return config;
    }
}

// `cuervo --dap` over stdio; the launch request carries project, backend and
// stopOnEntry.
class PlumaAdapterFactory {
    createDebugAdapterDescriptor(session) {
        const folder = session.workspaceFolder ? session.workspaceFolder.uri.fsPath : first_workspace_folder();
        const runtime = expand(settings().get('runtimePath', 'cuervo'), folder);
        return new vscode.DebugAdapterExecutable(runtime, ['--dap'], folder ? { cwd: folder } : undefined);
    }
}

async function activate(context) {
    output = vscode.window.createOutputChannel('Pluma');
    context.subscriptions.push(output);
    context.subscriptions.push(vscode.commands.registerCommand('pluma.restartServer', restart_client));
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration('pluma.toolsPath') || e.affectsConfiguration('pluma.projectPath') ||
                e.affectsConfiguration('pluma.checkStyle')) {
                restart_client();
            }
        })
    );
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('pluma', new PlumaConfigurationProvider())
    );
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('pluma', new PlumaAdapterFactory())
    );
    await start_client();
}

function deactivate() {
    return stop_client();
}

module.exports = { activate, deactivate };
