# Varian — VS Code

Syntax highlighting and full LSP for the Varian language and its frameworks
(Zenith HTTP, Lumen frontend, Aurora full-stack template).

## Features

- **Syntax highlighting** for `.vn`, `.vhtml`, and `.lumen` files (TextMate grammar).
- **Language server** (`vn lsp`): live diagnostics, hover, completion, go-to-definition, and formatting.

## Requirements

The `vn` toolchain must be installed and on your `PATH` (or set `varian.path` to its
location in settings). Get it from the [releases page](https://github.com/Chidi09/VarianLang/releases).

## Settings

| Setting        | Default | Description                                  |
| -------------- | ------- | -------------------------------------------- |
| `varian.path`  | `vn`    | Path to the `vn` executable.                 |

## Building / publishing this extension

```bash
cd editors/vscode
npm install
npx vsce package          # -> varian-0.1.0.vsix  (install via "Install from VSIX")
npx vsce publish          # VS Code Marketplace (needs a publisher + PAT)
npx ovsx publish *.vsix   # Open VSX (for VSCodium, Cursor, Gitpod, etc.)
```
