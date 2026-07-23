# Publishing the editor extensions

Goal: make Varian installable from the editor's marketplace (search "Varian" →
Install), exactly like any other language — no "install dev extension" dance.

Every marketplace requires a **publisher identity + token** the first time. This
is a one-time, ~5-minute setup (same as npm / PyPI / crates.io); it exists so
nobody else can push malware under the `varianlang` name. After that, releasing
is a single command.

---

## VS Code Marketplace

1. Create the publisher `varianlang` (once): https://marketplace.visualstudio.com/manage
   (sign in with a Microsoft account → New publisher → ID `varianlang`).
2. Create an Azure DevOps **Personal Access Token** with scope
   **Marketplace → Manage**: https://dev.azure.com → User settings → Personal access tokens.
3. Publish:

   ```sh
   cd editors/vscode
   npm install
   npx vsce login varianlang        # paste the PAT once
   npm run publish                  # or: npx vsce publish
   ```

   (Or non-interactive: `VSCE_PAT=<token> npx vsce publish`.)

## Open VSX  (Cursor, VSCodium, Windsurf, Gitpod, …)

The open marketplace most non-Microsoft editors use. Free, no Microsoft account.

1. Sign in at https://open-vsx.org with GitHub, create an access token, and
   create the `varianlang` namespace:

   ```sh
   npx ovsx create-namespace varianlang -p <OVSX_TOKEN>
   ```
2. Publish:

   ```sh
   cd editors/vscode
   npm run publish:ovsx -- -p <OVSX_TOKEN>     # or: OVSX_PAT=<token> npx ovsx publish
   ```

After both, users just search **Varian** in Extensions and click Install.

---

## Zed

Zed has no upload command — its registry is the **`zed-industries/extensions`**
repo, and it **builds every extension from source in CI**. Two pieces:

### `editors/zed-varian` (grammar, highlighting, themes, icons) — registry-ready
No native code; Zed builds the tree-sitter grammar from `Chidi09/tree-sitter-varian`.
To publish:

1. Fork `zed-industries/extensions`.
2. Add this repo as a submodule under `extensions/varian` and an entry in
   `extensions.toml`:

   ```toml
   [varian]
   submodule = "extensions/varian"
   path = "editors/zed-varian"
   version = "0.1.0"
   ```
3. Run their `./scripts/sort-extensions.sh`, commit, open a PR. Zed maintainers
   review + merge; then it's installable from Zed's Extensions panel.

### The `vn lsp` adapter — now part of `editors/zed-varian`
There is no longer a separate `zed-varian-lsp` extension. It was removed on
2026-07-23: Zed loaded it (`Loaded language server: varian-lsp` appears in the
remote-server log) but never once spawned `vn lsp` — no spawn attempt and no
error, across every log on the box. A language server declared in one extension
was not being matched to a language declared in another.

The adapter now lives in `editors/zed-varian/src/lib.rs`, declared by the same
`extension.toml` that declares the `Varian` and `Lumen` languages — the layout
every working extension uses (zig/zls, nix/nil, lua). It also supplies the Rust
source the old extension never had: it shipped a checked-in `extension.wasm`
with no source anywhere in the repo, which the registry would have rejected and
which nobody could rebuild or audit.

Build check (Zed compiles this itself on install, but this catches breakage
early):

```sh
rustup target add wasm32-wasip1
cd editors/zed-varian && cargo build --release --target wasm32-wasip1
```

> Until the registry PR merges, the Zed extension installs via the command
> palette → **`zed: install dev extension`** → pick `editors/zed-varian`.
> There is only ONE extension to install now. If you previously installed
> `varian-lsp`, **uninstall it first** — two extensions declaring the same
> server name will conflict.
