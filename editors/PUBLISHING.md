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

### The extension lives in its own repo: https://github.com/Chidi09/zed-varian

It is **not** in this repo. `editors/zed-varian/` was removed on 2026-07-23 and
moved out, because Zed's registry requires `extension.toml` at the repo root and
consumes the extension as a git submodule. Keeping a second copy here would only
let the two drift.

That repo contains the grammar reference, highlight queries, themes, icons, and
the Rust `vn lsp` adapter. Its README covers installing, pointing Zed at a
specific `vn` build, and validating highlight queries.

Also removed at the same time: the separate `zed-varian-lsp` extension. Zed
loaded it (`Loaded language server: varian-lsp` appears in the remote-server
log) but never once spawned `vn lsp` — no spawn attempt and no error, across
every log on the box. A language server declared in one extension was not being
matched to a language declared in another, so the server declaration now lives
in the same extension as the languages.

To publish to the registry:

1. Fork `zed-industries/extensions` (already forked: `Chidi09/extensions`).
2. Add `Chidi09/zed-varian` as a submodule under `extensions/varian`, plus an
   entry in `extensions.toml`:

   ```toml
   [varian]
   submodule = "extensions/varian"
   version = "0.1.0"
   ```

   No `path` key — that is only needed when the extension sits in a
   subdirectory of its repo, which it no longer does.
3. Run their `./scripts/sort-extensions.sh`, commit, open a PR. Zed maintainers
   review + merge; then it is installable from Zed's Extensions panel.

> Until that PR merges, install via the command palette →
> **`zed: install dev extension`** → pick a clone of `Chidi09/zed-varian`.
> If you previously installed `varian-lsp`, **uninstall it first** — two
> extensions declaring the same server name will conflict.
