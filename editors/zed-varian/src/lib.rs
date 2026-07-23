//! Zed extension for the Varian language: grammar, highlighting, themes,
//! icons, AND the `vn lsp` language server adapter.
//!
//! ─── Why this lives in the same extension as the language ───
//!
//! This adapter used to be a SEPARATE extension (`editors/zed-varian-lsp`),
//! which declared `[language_servers.varian-lsp] language = "Varian"` while the
//! `Varian` language itself was declared over here. Zed loaded that extension
//! ("Loaded language server: varian-lsp" appears in the remote-server log) but
//! never once spawned `vn lsp` — no spawn attempt, and no error either, across
//! every log going back weeks. Nothing was matching the server to a buffer.
//!
//! A language server and the language it serves are now declared by one
//! extension, which is how every extension that demonstrably works on this
//! setup (zig/zls, nix/nil, lua) is arranged. It also removes the reason the
//! split existed: the old note in extension.toml correctly observed that
//! declaring `[language_servers.*]` in an extension with NO Rust code makes Zed
//! fail to load it outright (which silently kills syntax highlighting too). The
//! answer to that is to give this extension Rust code, not to split it in two.
//!
//! Note that the previous extension's compiled `extension.wasm` was checked in
//! with no source anywhere in the repo, so it could not be rebuilt or audited.
//! This file is that missing source.

use zed_extension_api::{self as zed, Result};

/// The command we spawn. `vn lsp` speaks LSP over stdio (see `src/lsp.c`,
/// `lsp_main`), so no `--stdio` flag is needed or accepted.
const SERVER_ARG: &str = "lsp";

struct VarianExtension;

impl VarianExtension {
    /// Resolve the `vn` binary.
    ///
    /// Order matters: an explicit setting must win over PATH discovery, so a
    /// user working on the compiler itself can point Zed at the build they are
    /// testing rather than whichever `vn` happens to be installed.
    fn binary_path(
        &self,
        language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<String> {
        // 1. `lsp.varian-lsp.binary.path` in Zed settings.
        if let Ok(settings) = zed::settings::LspSettings::for_worktree(
            language_server_id.as_ref(),
            worktree,
        ) {
            if let Some(binary) = settings.binary {
                if let Some(path) = binary.path {
                    return Ok(path);
                }
            }
        }

        // 2. `vn` on the worktree's PATH.
        if let Some(path) = worktree.which("vn") {
            return Ok(path);
        }

        // 3. Give up with an actionable message rather than a silent no-op.
        //    A missing binary is by far the most likely cause of "the LSP does
        //    nothing", and Zed surfaces this string in the UI.
        Err(
            "could not find the `vn` binary on PATH. Install VarianLang, or set \
             `lsp.varian-lsp.binary.path` in your Zed settings to an absolute \
             path (e.g. /root/dev/VarianLang/vn)."
                .to_string(),
        )
    }
}

impl zed::Extension for VarianExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let command = self.binary_path(language_server_id, worktree)?;

        // Allow settings to append arguments, but always keep `lsp` first —
        // without it `vn` runs a different subcommand entirely and Zed would
        // sit waiting on a process that never speaks LSP.
        let mut args = vec![SERVER_ARG.to_string()];
        if let Ok(settings) = zed::settings::LspSettings::for_worktree(
            language_server_id.as_ref(),
            worktree,
        ) {
            if let Some(binary) = settings.binary {
                if let Some(extra) = binary.arguments {
                    args.extend(extra);
                }
            }
        }

        Ok(zed::Command {
            command,
            args,
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(VarianExtension);
