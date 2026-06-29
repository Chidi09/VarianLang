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

### `editors/zed-varian-lsp` (the `vn lsp` adapter) — needs Rust source first
This extension currently ships only a prebuilt `extension.wasm`. Zed's registry
**will not accept a checked-in wasm** — it compiles the adapter from Rust. Add a
crate next to `extension.toml` (and drop the `wasm = { path = … }` line so Zed
builds it):

```toml
# Cargo.toml
[package]
name = "zed_varian_lsp"
version = "0.1.0"
edition = "2021"
[lib]
crate-type = ["cdylib"]
[dependencies]
zed_extension_api = "0.2"   # pin to the version current Zed expects
```

```rust
// src/lib.rs
use zed_extension_api as zed;
struct Varian;
impl zed::Extension for Varian {
    fn new() -> Self { Varian }
    fn language_server_command(
        &mut self, _id: &zed::LanguageServerId, _wt: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        Ok(zed::Command { command: "vn".into(), args: vec!["lsp".into()], env: vec![] })
    }
}
zed::register_extension!(Varian);
```

Verify locally with the Zed toolchain (`zed: install dev extension`) before the
registry PR, since `zed_extension_api` versions change. Until then, the LSP
works as a one-time dev-extension install.

> Until the registry PRs merge, both Zed extensions install via the command
> palette → **`zed: install dev extension`** → pick `editors/zed-varian` then
> `editors/zed-varian-lsp`.
