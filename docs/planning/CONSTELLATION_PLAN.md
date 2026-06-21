# Constellation — the Varian package registry

**Packages are stars; Constellation is the map that makes them a navigable whole.** Part of
the ecosystem: Varian (language) · Zenith (backend) · Lumen (frontend) · Kiln (build) ·
**Constellation (registry)**. Status: **NOT BUILT YET — this is the design.**

The user-facing verbs are short (`vn add`, `vn install`, `vn publish`) — Constellation is
the conceptual umbrella, not a word anyone types, the same way `vn dev` fronts Lumen.

## Decided model — hybrid: a thin index + git, hash-pinned

(Chosen 2026-06-21.) The two extremes were rejected: a full hosted registry (npm/crates)
means running, funding, and moderating a real service; a purely decentralized scheme
(Go-style `vn add github.com/u/p`) means no short names and no central search. **Hybrid**
keeps the cost low and the UX good:

- You run **only a tiny static index** (a JSON file on a CDN / git-hosted) mapping a short
  name → its versions → `{ git url, tag, sha256, manifest }`. No binary hosting, no
  accounts, no database for v1.
- `vn add lumen-ui` hits the index, resolves a semver range, **fetches the tarball from
  git**, **verifies its `sha256`**, vendors it, and writes a lockfile.
- Code lives in normal git repos owned by their authors. The index just *points*.

```
vn add lumen-ui
  └─ index:  lumen-ui ^1.0  ->  github.com/you/lumen-ui  @ v1.2.0  sha256:abcd…
  └─ fetch tarball, verify sha256 == abcd…
  └─ vendor into vn_modules/lumen-ui/
  └─ write constellation.lock  (exact version + hash, reproducible)
```

## Charter — npm's reach, without npm's footguns

| npm / cargo pain | Constellation's answer |
| --- | --- |
| `node_modules` black hole, irreproducible installs | Hash-pinned **lockfile**; `vn install --frozen` reproduces exactly or fails loudly. |
| Supply-chain surprise (postinstall scripts run silently) | Native/build packages are **opt-in and consented**, sandboxed, and hash-recorded (below). |
| Registry infra cost & central point of failure | Thin index only; code is fetched from the authors' own git. |
| Dependency hell / phantom deps | Flat, explicit, vendored under `vn_modules/`; what you see is what builds. |
| Separate publish toolchain | One `vn` CLI: `vn add` / `vn install` / `vn update` / `vn publish` / `vn search`. |

## What exists today (honest baseline)

- **`varian.pkg`** — a minimal manifest (`[deps]`, `pkg = "latest"`) written by `vn add`.
- **`vn add <pkg>`** (`pkg_add` in `src/pkg_manager.c`) currently **only appends a text
  line** to `varian.pkg`. There is **no fetch, no version resolution, no lockfile, no
  registry** behind it. It is a stub.
- **`vn wrap python:<mod>`** generates a Varian wrapper from Python introspection — the
  seed of "a package can be generated," and the first thing worth being publishable.
- Vendoring target already exists conceptually: `vn_modules/` is the prelude dir.

## The hard prerequisite: a real module system

**This is the load-bearing honesty of this plan.** Today Varian has *no namespaced
imports* — `read_directory_sources` globs `vn_modules/*.vn` and **concatenates every file
into one global prelude**. That is fine for a handful of first-party stdlib modules. It
**does not scale to third-party packages**: two packages defining `fn parse()` silently
collide; load order becomes load-bearing; one package can clobber another's globals.

So Constellation cannot be "real" until Varian gains **namespaced modules** — a `use`
mechanism so a vendored package's symbols live behind its name (`lumen_ui.Button(...)`)
instead of in one flat global soup. This is milestone **C4**, and everything multi-package
depends on it. Pretending packages work on top of today's concatenation would be a half
measure; naming this gap up front is the point.

## Manifest & lockfile

**`constellation.toml`** (supersedes `varian.pkg`; a one-time `vn` migration reads the old
format and rewrites it):

```toml
[package]
name    = "lumen-ui"
version = "1.2.0"

[deps]
some-lib = "^2.1.0"            # semver range, resolved via the index
util     = { git = "github.com/u/util", tag = "v0.3.1" }   # direct git, index-free

# Required for native/FFI/Python packages — declares what the package may touch.
[capabilities]
ffi    = false
python = false
net    = false
fs     = false

[build]                        # only if the package has a native/build step
script = "build.vn"
```

**`constellation.lock`** records, for every transitive package: resolved exact version, the
git URL + commit, the `sha256` of the fetched tarball, and (for build packages) the hash of
the build script. `vn install` restores from it; `vn install --frozen` verifies and refuses
any drift. Reproducibility is a property of the lockfile, never of the network.

## The native-package trust model (the chosen, riskier scope)

Allowing packages to ship C/FFI/Python code and build steps is powerful and was explicitly
chosen — so the safety has to be designed in, not bolted on. The rule: **native code never
runs as a silent side effect of `vn add`.**

1. **Capabilities are declared, not discovered.** A package's `[capabilities]` lists what it
   needs (`ffi`, `python`, `net`, `fs`). `vn add` surfaces them; a build that tries to use a
   capability it didn't declare is denied.
2. **Builds are consented and recorded.** Installing a package with a `[build]` step
   prompts for explicit trust on first add (or `--allow-build` in CI). The decision +
   the build-script hash are written to the lockfile; if the script later changes, trust is
   re-prompted (trust-on-first-use, pinned).
3. **Builds run constrained.** The build step runs through Kiln in a restricted environment
   (no ambient network unless `net` was granted; writes confined to the package's build
   dir). Best-effort sandboxing with the platform's tools, documented honestly about what it
   does and does not guarantee — mirroring `docs/SECURITY.md`'s sandboxing caveat rather
   than overclaiming.
4. **Everything is hash-pinned.** Tarball `sha256` + build-script hash in the lockfile mean
   a swapped artifact upstream fails verification instead of executing.

Pure-Varian packages (no `[build]`, no capabilities) skip all of this — they're just
vendored source, the common and safe case.

## Milestones (all PLANNED)

- **C0 — `constellation.toml` + migration.** Define the manifest, teach `vn` to read it
  (and migrate `varian.pkg`). No network yet. Mechanical.
- **C1 — lockfile + `vn install`.** Given a manifest with direct-git deps, fetch, verify
  `sha256`, vendor under `vn_modules/<pkg>/`, write `constellation.lock`. `vn install`
  restores; `--frozen` verifies. (Direct-git only — no index needed yet.)
- **C2 — fetch & integrity layer.** Robust git/tarball fetch, hash verification, vendor
  layout, `vn remove`, `vn update` (re-resolve within ranges, rewrite lock).
- **C3 — the thin index + semver.** Stand up the static index format + a resolver so
  `vn add <shortname>` works (name → versions → git+hash). `vn search` over the index.
- **C4 — namespaced modules (THE prerequisite, biggest item).** `use "<pkg>"` ships (flat
  global, with hard collision detection); the remaining piece is per-package isolation via
  `use "<pkg>" as ns` → `ns.symbol`. **Full design in [`NAMESPACING_PLAN.md`](NAMESPACING_PLAN.md)**:
  a module compiles to a struct built by an init function (package fns become nested locals →
  siblings resolve locally → isolation), reusing closures + structs + `BC_MEMBER` with no new
  opcode; the one real compiler change is hoisting module locals for mutual reference. Functions
  and values namespace in v1; types stay global (collision-checked) until a scope-aware type
  resolver lands.
- **C5 — native / FFI / Python packages.** The trust model above: capability manifest,
  consented + sandboxed builds via Kiln, build-script hashing. Only after C4 (namespacing)
  and Kiln K0–K2 (so there's a real build to sandbox).
- **C6 — `vn publish`.** Push a new version to the index (open a PR against the index repo,
  or a minimal authenticated endpoint if/when the index outgrows being purely static).
- **C7 — signing / provenance (DEFERRED, honestly).** Sigstore-style signatures and a
  provenance trail are the right long-term integrity story but are not a v1 promise; the
  `sha256` lockfile is the v1 integrity floor. Do it demand-driven.

## How it fits Kiln

Constellation produces `constellation.lock`; **Kiln consumes it** with no network at build
time (see `KILN_PLAN.md`). The split is deliberate: resolution/trust decisions happen at
`vn add` time (interactive, online); builds are a pure, offline, reproducible function of
the locked set. C4 (modules) and Kiln K0 (`libvarian.a`) are the two foundational unlocks
for the whole ecosystem — recommend landing those before the higher milestones of either
tool.

## Open decisions (revisit when reached)

- Index hosting: a git repo of JSON (zero infra, PR-based publish) vs. a tiny read-only
  service. Start with the git-repo-of-JSON; it needs no servers.
- Vendoring vs. a global content-addressed store (à la pnpm): vendoring is simpler and more
  auditable for v1; revisit if disk duplication becomes a real complaint.
- Namespacing syntax (C4): `use pkg` exposing `pkg.symbol`, vs. selective `use pkg.{a, b}`.
  Decide with the language's existing feel; don't make users relearn imports.
- Whether `vn wrap` output (Python/FFI wrappers) becomes the first official publishable
  package type — a natural seed for C5.
