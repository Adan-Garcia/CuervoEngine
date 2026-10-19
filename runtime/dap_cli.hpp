#pragma once

// `cuervo --dap`: the runtime as a Debug Adapter Protocol server on stdin /
// stdout (DESIGN.md 5.14), the way the VS Code extension (tools/vscode-pluma)
// starts it. The `launch` request's arguments pick the game:
//
//   "project": "<dir with project.toml>"   (required)
//   "backend": "vulkan" | "gl"             (optional)
//   "stopOnEntry": true                    (stop at the first script line)
//   "noDebug": true                        (just run it)
//
// Script `print` and the engine log become `output` events; nothing else
// is written to stdout. Breakpoints, stepping and inspection work on the
// bytecode VM (development runs; not AOT-native packs). Desktop only.
int run_dap(int argc, char** argv);
