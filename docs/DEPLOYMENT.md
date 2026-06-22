# Deployment, Releases & Editor Support

How Varian ships: the toolchain release, the editor extensions, and hosting a
Lumen/Aurora site (including the honest answer on Vercel).

---

## 1. Releasing the toolchain (AppVeyor → GitHub Releases)

**This is already wired up** in `appveyor.yml`. One binary is the whole toolchain:
`vn` is the language runtime **and** Kiln (`vn build`) **and** Constellation
(`vn add`/`publish`/`search`/`install`) **and** the LSP (`vn lsp`). They are not
separate downloads.

The release tarball (`varian-linux-x64.tar.gz`) contains:
- `vn` — the binary
- `libvarian.a` + `include/` — needed for `vn build --release` (native compilation)
- `vn_modules/` — the standard-library prelude + Lumen assets (loaded at runtime)
- `README.md`, `LICENSE`

### Cutting a release (your steps — releases are outward-facing, so they're not automated away)

1. **One-time:** create a GitHub personal access token (`public_repo` scope), then in
   AppVeyor → *Account → Encrypt YAML*, encrypt it and paste the result into
   `deploy.auth_token.secure` in `appveyor.yml` (currently the placeholder
   `REPLACE_WITH_APPVEYOR_ENCRYPTED_GITHUB_TOKEN`).
2. Bump the version (`VARIAN_VERSION` in `src/main.c` + `include/varian.h`, and the
   `version:` line in `appveyor.yml`).
3. Tag and push:
   ```bash
   git tag v0.1.0
   git push origin v0.1.0
   ```
   AppVeyor builds the hardened binary, runs the full test suite + package/namespacing
   integration suites, packages the tarball + SHA-256, and publishes a GitHub Release.

### Other platforms
- **Windows:** AppVeyor can add a Windows image to the matrix (the binary already builds
  there per prior CI work). `liburing` is Linux-only, so the Windows/macOS event loop path
  differs — keep the Linux artifact as the primary until the others are verified end-to-end.
- **macOS:** same `make release`, but swap `liburing-dev` (Linux) out; verify before
  advertising a macOS artifact rather than assuming it builds.

---

## 2. Editor support (syntax + LSP "when downloaded")

All editors share one language server: `vn lsp` (stdio JSON-RPC) — diagnostics, hover,
completion, go-to-definition, and formatting. Syntax highlighting is per-editor.

### VS Code (and Cursor / VSCodium / Gitpod via Open VSX)
`editors/vscode/` is a complete extension:
- TextMate grammar (`syntaxes/varian.tmLanguage.json`) → highlighting for `.vn`/`.vhtml`/`.lumen`
- `language-configuration.json` → comments, brackets, auto-close, indentation
- LSP client (`extension.js`) → spawns `vn lsp`
- **File icon** for `.vn`/`.lumen` files via `contributes.languages[].icon` (`icons/varian-*.svg`)
- **Marketplace tile** via `package.json` `"icon"` (`icon.png`, the Aurora mark)

Build / publish:
```bash
cd editors/vscode
npm install
npx vsce package                 # -> varian-0.1.0.vsix (drag-drop install, or "Install from VSIX")
npx vsce publish                 # VS Code Marketplace — needs a `varianlang` publisher + Azure DevOps PAT
npx ovsx publish *.vsix -p $TOK  # Open VSX — covers Cursor, VSCodium, Gitpod, Theia
```
> Publishing to a marketplace requires accounts/tokens only you can create; the extension
> itself is ready to package and side-load today.

### Zed
`editors/zed/` has `extension.toml` + `languages/varian/{config.toml,highlights.scm}`.
Zed highlighting requires a **tree-sitter grammar** (not TextMate). The remaining step is
to publish a `tree-sitter-varian` grammar repo and pin its commit in `extension.toml`
(`[grammars.varian] rev = ...`). Until then, Zed gets the LSP (diagnostics/hover/etc.) but
not local highlighting. The `highlights.scm` queries are already written against the
grammar's expected node names, so once the grammar exists, highlighting works immediately.
Install during development via Zed → *Extensions → Install Dev Extension* → pick `editors/zed`.

### Neovim / other LSP clients
Point any LSP client at `vn lsp`. Example (nvim-lspconfig):
```lua
require('lspconfig.configs').varian = { default_config = {
  cmd = {'vn','lsp'}, filetypes = {'varian'}, root_dir = require('lspconfig.util').find_git_ancestor } }
require('lspconfig').varian.setup{}
```
Highlighting reuses the same tree-sitter grammar as Zed.

### Language icons — how they work
There are **two** icons, and they're configured in different places:
1. **File-explorer glyph** (next to files): `contributes.languages[].icon` is *per language*,
   so to give `.lumen` files the Lumen mark and `.vn` files the Varian mark we register **two
   languages** that share one grammar + LSP:
   - `varian` → `.vn`, `.vhtml` → Aurora/V mark (`icons/varian-{light,dark}.svg`)
   - `lumen` → `.lumen` → amber bolt (`icons/lumen-{light,dark}.svg`)
   Shows in file-icon themes that defer to language icons (the VS Code default "Seti" theme does).
2. **Marketplace/extension tile**: top-level `"icon"` in `package.json` → a ≥128×128 PNG.
   Done — `icon.png` (256×256, the Aurora mark, generated from `website/public/aurora-logo.png`).

To regenerate the marketplace PNG from a logo:
```bash
ffmpeg -i website/public/aurora-logo.png \
  -vf "scale=256:256:force_original_aspect_ratio=increase,crop=256:256" editors/vscode/icon.png
```

---

## 3. Hosting the website — Vercel vs. the live server

**Short answer: Vercel cannot run the Varian site as-is.** The Lumen site is a *live
server* — a long-running `vn` process that holds a WebSocket per client and re-renders on
the server (server-driven reactivity). Vercel is serverless/static: no persistent process,
no long-lived WebSocket. So you have two real paths.

### Path A (recommended) — run the real server (full interactivity)
Use a host that runs a long-lived container: **Fly.io, Railway, Render, or any VPS**. A
production `Dockerfile` is at `website/Dockerfile` (builds `vn`, compiles the pages with
`vn lumen build`, runs `vn run`).

```bash
docker build -f website/Dockerfile -t varian-site .
docker run -p 8080:8080 varian-site
```
- **Fly.io:** `fly launch --dockerfile website/Dockerfile`, set `internal_port = 8080` in
  `fly.toml`. (WebSockets supported.)
- **Railway/Render:** point the service at the Dockerfile; expose port 8080.

This keeps the live console, server-driven `@click` handlers, and theming all working.

### Path B — static export to Vercel (content only, no live features)
The marketing pages are mostly static content; if you only need that, render each route to
a static `.html` and deploy the `public/` assets + rendered HTML to Vercel as a **static
site** (no serverless functions). What you lose: anything that round-trips to the server
(the live runnable console, server-driven handlers). Client-only JS (theme toggle, the
LumenJS modules that don't hit the socket) still works.

There is **no `vn` "export to static HTML" command yet** — Path B needs a small exporter
(render each page through the Lumen pipeline, strip the live-socket `<script>`, write
`route.html`). If you want Path B, that exporter is a focused follow-up I can build; then
deploy is just:
```bash
# after the exporter writes ./public_static/*.html + assets
vercel deploy --prebuilt ./public_static    # or set Output Directory in the Vercel dashboard
```
A `vercel.json` would set clean routes/headers. But for a "new language" site where the
interactive console is the selling point, **Path A is the right call** — the console only
works with the live server.

---

## TL;DR
- **Release:** already automated in `appveyor.yml` → add the encrypted GitHub token once, then `git tag vX && git push --tags`. One binary = runtime + Kiln + Constellation + LSP.
- **Editors:** VS Code extension is complete (highlighting + LSP + file icon + marketplace icon) and ready to `vsce package`. Zed has LSP now; highlighting needs a `tree-sitter-varian` grammar repo. Any LSP client works via `vn lsp`.
- **Website on Vercel:** not directly — it's a live server. Host the container (Fly.io/Railway/VPS) for full features (`website/Dockerfile`), or build a static exporter for a content-only Vercel deploy.
