# Constellation — The Varian Package Registry & Dependency Manager

**Packages are stars; Constellation is the map that makes them a navigable whole.**

Constellation is Varian's built-in package management system. It handles dependency declaration, static registry searches, semver compatibility matching, reproducible offline builds, and transitive package resolution with integrity hashing.

---

## 1. Architecture: Hybrid CDN Index + Git Vendoring

Constellation avoids the high maintenance costs and security vulnerabilities of fully hosted central registries (like npm or crates.io) by utilizing a **hybrid model**:
* **Thin Static Index**: Package names are mapped to version structures in a single static JSON index (which can be hosted on a CDN, a Git repository, or a local file during offline tests). Each entry contains:
  $$\text{Version} \longrightarrow \{\text{git url}, \text{git tag}, \text{sha256 tarball hash}\}$$
* **Direct Git Referencing**: Dependencies can bypass the registry index entirely and point directly to a Git repository URL and a specific branch/tag.
* **Local Vendoring**: Dependencies are unpacked into `vn_modules/<package_name>` locally. This provides absolute visibility into third-party code and ensures your project compiles offline.

```
                    constellation.toml
                           │
             ┌─────────────┴─────────────┐
             ▼                           ▼
      Registry Spec                  Direct Git
      ("lumen-ui" = "^1.2.0")        (git = "...", tag = "...")
             │                           │
             ▼                           │
      CDN Registry Index                 │
      (Match best Semver)                │
             │                           │
             ▼                           ▼
      ┌─────────────────────────────────────┐
      │ Fetch Tarball from Git repository   │
      └──────────────────┬──────────────────┘
                         │
                         ▼
      ┌─────────────────────────────────────┐
      │ Compute & Verify SHA-256 Checksum   │
      └──────────────────┬──────────────────┘
                         │
                         ▼
      ┌─────────────────────────────────────┐
      │ Unpack into vn_modules/<package>    │
      └──────────────────┬──────────────────┘
                         │
                         ▼
                 constellation.lock
```

---

## 2. Manifests & Version Pinning

### The Manifest: `constellation.toml`
The configuration for your project lives in `constellation.toml` (which automatically replaces the legacy `varian.pkg` format). Below is a complete specification of a package configuration:

```toml
[package]
name    = "my-lumen-app"
version = "0.1.0"

[deps]
# Registry-based dependency (resolved via the index using semver range matching)
lumen-ui = "^1.2.0"

# Direct Git dependency (index-free, pinned to a specific git location and tag)
auth-utils = { git = "github.com/varian-lang/auth-utils", tag = "v2.1.0" }

[capabilities]
# Capability declarations restrict what the compiled module is allowed to touch
ffi    = false
python = false
net    = true
fs     = true
```

### Semver Range Matching rules
Varian supports the following semver range selectors:
* **Caret (`^X.Y.Z`)**: Matches versions that do not modify the leftmost non-zero digit.
  * `^1.2.3` matches $\ge 1.2.3$ and $< 2.0.0$.
  * `^0.2.3` matches $\ge 0.2.3$ and $< 0.3.0$.
  * `^0.0.3` matches exactly $0.0.3$.
* **Tilde (`~X.Y.Z`)**: Matches versions within the same minor version block.
  * `~1.2.3` matches $\ge 1.2.3$ and $< 1.3.0$.
* **Wildcards (`*` or `latest`)**: Matches the absolute latest version available in the index.
* **Exact match (`X.Y.Z`)**: Matches the version string exactly.

### The Lockfile: `constellation.lock`
`constellation.lock` records the resolved state of the entire transitive dependency tree. It ensures that subsequent installs yield byte-identical code across different environments:

```toml
[[package]]
name = "dep_a"
version = "v1.2.0"
git = "/tmp/varian_pkg_c3_test/dep_a"
commit = "c040e9bc0868fbd1c9258285590c6b0c20202020"
sha256 = "d225ab17df20b9258238128ea3074d221804f326a203f42621743a6d258bf81f"

[[package]]
name = "dep_b"
version = "v0.1.0"
git = "/tmp/varian_pkg_c3_test/dep_b"
commit = "6b3771fb5d688cfd3d8204689cf08e8ef6e61203"
sha256 = "f339cf0120ab23a6c221bc014a6e336b28120023f054acb3815e61203fa8a32d"
```

---

## 3. CLI Command Reference

### `vn add <pkg_name>`
Adds a new dependency to `constellation.toml` at version `"latest"`. If no manifest exists, it automatically initializes a new one.

### `vn remove <pkg_name>`
Removes the specified dependency from `constellation.toml`, prunes its files and any unused transitive dependencies from the `vn_modules/` folder, and updates the `constellation.lock` file.

### `vn install [--frozen]`
Resolves and downloads all dependencies declared in `constellation.toml`.
* By default, it writes `constellation.lock` after the initial resolution.
* **Transitive Resolution**: Recursively parses `constellation.toml` manifests within downloaded package directories to construct the complete dependency tree.
* **Pruning**: Unused packages are deleted from `vn_modules/`.
* **`--frozen`**: Disallows network queries or lockfile updates. If the lockfile is missing or does not match `constellation.toml` exactly, the installation fails loudly (critical for CI pipelines).

### `vn update`
Bypasses the current `constellation.lock` cache and forces a fresh query to the Registry Index or Git repository, pulling the latest matching tags and generating a new lockfile with updated commits and SHA-256 hashes.

### `vn search <query>`
Queries the Registry Index and prints matching packages along with their available versions. Passing `"*"` lists every package in the index.

### `vn publish`
Prepares the current package (run from inside its git repo) for the index. It runs `git archive` on `HEAD`, computes the tarball SHA-256, and prints the exact index entry to submit. It does **not** upload anything — Constellation is a thin git-hosted index, so publishing means opening a PR that adds your entry; your own git host serves the code. Tag the commit `v<version>` and push so the hash stays valid.

---

## 4. Consuming a package: `use`
 
 Installing vendors a package under `vn_modules/<pkg>/`. The ambient prelude only auto-loads top-level `vn_modules/*.vn` (the standard library), so a vendored package is **loaded on demand** with an explicit `use` directive — your file declares exactly what it depends on.
 
- `use "lumen-ui"`: loads the package's top-level symbols into the **flat global namespace**. Collisions are *detected* (defining an in-scope name is a hard compile-time error) but not *avoided*.
- `use "lumen-ui" as ui`: loads the package into a **namespaced scope**. Accessing its symbols is done via member access, e.g., `ui.Button(...)` and `ui.Card(...)`.
- `use "lib/local.vn" as helpers`: accepts direct file paths as well.

### How Namespacing Works

Namespacing is grounded in existing runtime features without introducing new opcodes or VM types:
1. **Module Initialization Function**: When `use "pkg" as alias` is compiled, the compiler synthesizes a `__module_init_<pkg>()` function containing all the package's top-level declarations verbatim, returning an anonymous struct of its public names.
2. **True Isolation via Closures**: Sibling functions inside the package become nested functions inside the module initialization function. They resolve sibling references to enclosing locals/upvalues (e.g. `Button` calling `_render` resolves to a local sibling slot, not a global), achieving true per-package symbol isolation.
3. **Hoist-then-Define Pass**: To support mutually-referencing package functions declared out of order, the compiler performs a two-pass hoisting compile specifically inside the module initialization function. In Pass 1, all functions and variables are pre-declared as local slots. In Pass 2, their initializers and bodies are compiled and assigned to their slots. Sibling references then resolve correctly even before the target is defined in source order.
4. **Upvalue Closing**: When returning from a call frame, the VM copies final slot values to any closures that captured them (closing the upvalues) ensuring they remain correct after the module init frame is popped.
5. **Member Access**: The alias global is bound to the exported struct, and calling a function `ui.Button(...)` is compiled using the existing `BC_MEMBER` member-lookup instruction + call.

### Privacy Rule

Top-level package names starting with an underscore `_` (e.g., `_render_impl`) are visible to sibling functions inside the package but are **excluded from the exported struct**. Thus, `ui._render_impl` does not exist and throws a compile/runtime error on access. This aligns with standard library conventions (`_lumen_*`) without introducing new keywords.

### v1 Limitations (Types)

In version 1, namespacing applies to **functions and values** (the common case). Struct/enum type declarations (e.g., `struct Card`) are resolved at parse time into a global registry; making `ui.Card` usable as a type annotation requires a scope-aware type resolver. Therefore, types exported under a namespace remain global and collision-checked.

### Compilation Support

Namespacing is supported across all three Varian backends:
1. **Source Execution (`vn run main.vn`)**
2. **VNB Bundles (`vn build main.vn`)**
3. **Native Compiled Binaries (`vn build main.vn --release app`)**

The AOT compiler (`src/aot.c`) translates namespaced closures, metadata-based closure creation (`BC_CLOSURE`), and return-time upvalue closing (`close_upvalues`) directly into C, matching the interpreter's behavior exactly.

### `vn add` spec forms

```sh
vn add foo            # registry, "latest"
vn add foo@1.2.0      # registry, exact version
vn add foo@^1.2.0     # registry, semver range
vn add github.com/u/foo          # git dependency (tag "main")
vn add github.com/u/foo@v1.2.0   # git dependency at a tag
```

---

## 5. Transitive Resolution & Security Integrity

1. **Deterministic Dependency Ordering**: Dependencies are evaluated using a queue-based recursive resolver that avoids duplicate resolution cycles or infinite recursion loops.
2. **SHA-256 Validation**: Downloaded tarball archives are validated against the index-declared hash before extraction. This guarantees that files have not been modified or hijacked on the Git provider.
3. **Consented capabilities & build scripts**: A package that declares native capabilities (`ffi`/`python`/`net`/`fs`) or a `[build]` script cannot be installed silently. `vn install` lists what it requests and requires explicit consent — an interactive `y/N` prompt, or `CONSTELLATION_TRUST=1` for CI. Refusal removes the just-vendored files so nothing untrusted is left on disk. On approval the build script **runs** (via the `vn` runtime, from the package's own dir) and is then deleted from the vendored tree so it can't re-run at import time; a build failure aborts the install. The approved build-script hash is pinned in `constellation.lock` (trust-on-first-use). *Consent is the security boundary today, like npm's postinstall; full capability sandboxing of the build step is the future hardening.*
4. **Version conflicts fail loudly**: if two packages in the dependency graph require incompatible versions of the same package (different major, or two different exact pins), `vn install` stops with an error naming both requirers — never a silent first-wins resolution.
