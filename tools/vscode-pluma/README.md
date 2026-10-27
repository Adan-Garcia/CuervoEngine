# Pluma for VS Code

Language support for [Cuervo Engine](../../README.md) games written in Pluma (`.plm`):

- **Syntax highlighting**: keywords, `#names`, `@attributes`, strings with `{}`
  interpolation, raw (`r"..."`) and multiline (`"""..."""`) strings, numbers with
  suffixes (`45deg`, `2u8`, `1.5f64`), nested block comments, types and constants.
- **Language server** (`cuervo-tools lsp`): errors and style warnings as you type,
  completion (including the engine API), hover with doc comments, signature help, go to
  definition, find references, rename, document/workspace symbols, formatting, quick fixes.
- **Debugger** (`cuervo --dap`): line breakpoints in `.plm` files, call stack, locals,
  stepping. It debugs the bytecode VM (`cuervo --game`), not AOT-native exports.

The extension is a thin client: it needs the engine's `cuervo-tools` and `cuervo`
binaries (build the engine first, see [docs/GUIDE.md](../../docs/GUIDE.md)).

## Install

From this directory (`tools/vscode-pluma`):

```sh
npm install
npx @vscode/vsce package --allow-missing-repository --skip-license   # writes pluma-0.1.0.vsix
code --install-extension pluma-*.vsix
```

Or, for development, link the directory into VS Code's extensions folder and reload the
window (run `npm install` first so `node_modules/` exists):

```sh
ln -s "$PWD" ~/.vscode/extensions/cuervo.pluma-0.2.1
```

## Settings

| Setting | Default | |
|---|---|---|
| `pluma.toolsPath` | `cuervo-tools` | The `cuervo-tools` binary for the language server. A bare name is looked up on `PATH`; `${workspaceFolder}` and a leading `~` are expanded |
| `pluma.runtimePath` | `cuervo` | The `cuervo` runtime the debugger launches |
| `pluma.projectPath` | (empty) | The project directory (with `project.toml`) to analyze; empty means the first workspace folder |
| `pluma.checkStyle` | `true` | Naming-style warnings (LANGUAGE.md §6) |

Example, for a project inside the engine checkout built with the `debug` preset:

```json
{
  "pluma.toolsPath": "${workspaceFolder}/build/debug/tools/cuervo_tools/cuervo-tools",
  "pluma.runtimePath": "${workspaceFolder}/build/debug/runtime/cuervo",
  "pluma.projectPath": "${workspaceFolder}/examples/tutorial"
}
```

*Pluma: Restart Language Server* restarts the server (it also restarts when these
settings change).

## Debugging

Add `.vscode/launch.json` (or pick *Pluma (Cuervo): Launch* from *Add Configuration...*):

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "pluma",
      "request": "launch",
      "name": "Run game (Pluma)",
      "project": "${workspaceFolder}"
    }
  ]
}
```

| Attribute | |
|---|---|
| `project` | The game's project directory (default `${workspaceFolder}`) |
| `backend` | `"vulkan"` or `"gl"` (optional; default Vulkan, falling back to GL) |
| `stopOnEntry` | Pause before the first tick (default `false`) |

Set breakpoints in `.plm` files and press F5. Without a `launch.json`, F5 on a `.plm`
file launches the workspace folder's game.

## Files

- `extension.js`: the language client and the debug adapter registration (plain
  JavaScript, no build step)
- `syntaxes/pluma.tmLanguage.json`: the TextMate grammar
- `language-configuration.json`: comments, brackets, auto-closing, indentation
