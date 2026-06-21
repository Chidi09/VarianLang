# Kiln — the Varian build tool

**Kiln fires raw Varian into a hardened, shippable artifact.** Part of the ecosystem:
Varian (language) · Zenith (backend) · Lumen (frontend) · **Kiln (build)** · Constellation
(registry). Status: **NOT BUILT YET — this is the design.**

The user-facing command is `vn build` (Kiln is the conceptual umbrella, the same way
`vn dev` is the user-facing verb for Lumen). The name's metaphor — heat that hardens clay
into something durable — is the whole job: take loose `.vn`/`.lumen` source + resolved
dependencies and produce one artifact you can run or deploy with nothing else installed.

## Charter — one command, one artifact, zero config

Measured against the tools it replaces, every Kiln decision removes a specific papercut:

| webpack/vite/tsc/cargo/go build pain | Kiln's answer |
| --- | --- |
| Config sprawl (webpack.config, tsconfig, vite.config, build.rs) | **Zero config.** `vn build`. Targets come from `constellation.toml`, with sane defaults. |
| Slow cold builds, no caching | Content-addressed build cache: unchanged inputs are never recompiled. |
| Heavy toolchain just to run (node_modules + bundler) | Default output is a single portable bundle the `vn` binary runs directly — no toolchain. |
| "Works on my machine" / non-reproducible builds | Builds are a pure function of (locked deps + source + flags); same inputs → same artifact. |
| Separate steps to bundle a web app's assets/pages | Kiln folds in `vn lumen build` (pages → routes) and embeds `public/` assets. |
| Native speed needs a separate compile pipeline | `--release` drives the existing AOT path to a real native binary, same command. |

## What exists today (honest baseline)

Grounding the plan in the actual codebase, not aspiration:

- **`vn compile <file.vn> [out.c]`** (`src/aot.c`, `aot_compile`) emits a **`.c` file**
  containing per-function AOT C functions plus a `varian_aot_load(VM*)` loader. It is
  **not** a standalone program — there is no emitted `main()`, and nothing links it. A
  human currently has to hand-write a harness and a `cc` command. Turning that `.c` into a
  runnable binary is unspecified and manual today.
- The runtime is built as **scattered object files** (`build/*.o` linked straight into
  `vn`); there is **no `libvarian.a`** for AOT output to link against.
- **`vn lumen build <dir> <out.vn>`** already bundles a `pages/` dir into one runnable
  `.vn` — a working prototype of one Kiln backend, to be folded in.
- The "module system" is **whole-file prelude concatenation** (`read_directory_sources`
  in `src/main.c` globs `vn_modules/*.vn` and concatenates them before the entry file).
  There is no compiled-bytecode container format and no incremental compilation.
- A historical note in `src/vm.c` (`Phase 2: per-request arena — DISABLED for AOT
  debugging`) records that AOT and the arena allocator have conflicted before. **The
  `--release` native path must be validated empirically, not assumed working.**

## Architecture — a build graph with two backends

```
constellation.lock ─┐
                     ├─▶ resolve  ─▶ assemble sources ─▶ compile ─┬─▶ [bundle]  app.vnb
project .vn/.lumen ─┘   (vendored     (prelude + deps +   (lex/    │   (serialized bytecode,
public/ assets ─────────deps from      project + pages)    parse/   │    run by vn, no toolchain)
                        Constellation)                      compile) └─▶ [release] ./app
                                                                          (AOT .c → cc → native +
                                                                           embedded assets)
```

1. **Resolve.** Read `constellation.lock` (from Constellation) for the exact, hash-pinned
   dependency set. No network in `vn build`; resolution happened at `vn add` time.
2. **Assemble.** Concatenate/namespace the prelude + vendored deps + project sources, and
   compile any `.lumen` pages to their backing `.vn` (reusing `_lumen_build_dir`).
3. **Compile to one of two artifacts** (below).

### Backend A — portable bundle (`vn build`, the default)

Output: a single **`app.vnb`** container. Run with `vn app.vnb` (or just `./app.vnb` via a
shebang). No C toolchain required anywhere — it runs wherever the `vn` runtime is installed.

This needs a **bytecode container format** that does not exist yet:

- A versioned header (magic + format version + runtime-ABI version), the serialized
  function chunks (code + constants, the same data `aot.c` already knows how to walk), the
  string table, and any embedded assets (Lumen `public/`).
- A loader fast-path in `vn`: when the input is a `.vnb`, **skip lex/parse/compile entirely**
  and deserialize chunks straight into the VM. This is also a meaningful startup-latency win
  for large apps (no re-parsing the whole prelude every launch).
- Format-version gating: a `.vnb` built by an incompatible runtime must refuse to run with a
  clear error, never silently misinterpret bytes.

### Backend B — native binary (`vn build --release`)

Output: a standalone executable `./app`. Drives the existing AOT pipeline end to end:

1. `aot_compile()` the assembled program to C (exists).
2. Emit the missing **`main()` harness** that calls `varian_aot_load(vm)` and runs `fn_0`.
3. Invoke the system C compiler (`cc`, overridable via `$CC`) to compile that C and **link
   it against `libvarian.a`** (see K0) + the same external libs the main binary uses
   (`-lm -lffi -ldl -lcurl -lpq -lcrypto -lssl -lsqlite3 -lhiredis -lpthread -luring`).
4. Embed `public/` assets into the binary (or emit them beside it) so a Lumen/Zenith app
   ships as one deployable unit.

`--release` requires a C toolchain on the build machine — that is the explicit, documented
cost of the native path, and exactly why it is opt-in and the bundle is the default.

## What Kiln reads from `constellation.toml`

```toml
[package]
name = "myapp"
version = "0.1.0"

[build]
entry  = "main.vn"      # or pages = "pages" for a Lumen app
out    = "app"          # artifact basename
assets = ["public"]     # dirs embedded into the artifact
```

Defaults make the manifest optional for simple apps: `entry` defaults to `main.vn`; a
`pages/` dir present → Lumen app mode; `public/` embedded if present.

## Incremental build cache

Builds are keyed by a content hash of (each input file's bytes + locked dep set + Kiln
version + flags). Compiled chunks are cached under `.kiln/cache/<hash>`; an unchanged file
is never recompiled. The cache is purely an optimization — a cold `.kiln/` always yields a
byte-identical artifact (reproducibility is not cache-dependent).

## Milestones (all PLANNED)

Each must pass the charter test: does it make `vn build` more turnkey *without* adding a
concept the user has to configure?

- **K0 — `libvarian.a` (prerequisite).** Add a Makefile target that archives the runtime
  objects (everything except `main.o`) into `libvarian.a`, and ship the public headers.
  Nothing user-visible; it's the thing AOT output links against. Without this, `--release`
  can't produce a binary turnkey. Low risk, mechanical.
- **K1 — bundle backend (`vn build`).** Define the `.vnb` container format + serializer +
  the `vn` loader fast-path. Wire `vn build` (no flag) to assemble → compile → write
  `app.vnb`. Verify `vn app.vnb` runs byte-identically to `vn run main.vn`.
- **K2 — native backend (`vn build --release`).** Emit the `main()` harness, drive
  `aot_compile` + `cc` + link against `libvarian.a`, produce `./app`. **Gate:** prove the
  AOT/arena interaction from the historical "disabled for AOT debugging" note is resolved
  on a struct-heavy program before calling this done; if it regresses, stop and report.
- **K3 — build cache.** Content-addressed `.kiln/cache`. Measure cold vs warm build on a
  realistic app; document the speedup honestly.
- **K4 — web integration.** Fold `vn lumen build` into `vn build` (pages → routes), embed
  `public/` assets into both backends, and make a Zenith/Lumen app a one-artifact deploy.
- **K5 — static / cross builds (DEFERRED, honestly).** Fully static (musl) and
  cross-compilation are valuable for deployment but multiply toolchain complexity. Do them
  benchmark/demand-driven, after K1–K4 prove the single-target path, the same way Zenith's
  native paths were earned. Not a v1 promise.

## Open decisions (revisit when reached)

- `.vnb` format: roll our own minimal container vs. a documented framing over the existing
  `aot.c` serialization. Lean minimal-and-versioned; do not over-design v1.
- Whether `vn build` (bundle) and `vn build --release` (native) should share one assembled
  IR or each re-assemble. Share, to guarantee the two artifacts are behaviorally identical.
- Asset embedding: bytes-in-binary vs. a sidecar dir. Embed for true single-file deploy;
  allow `--assets=external` for large media.
- Sequencing: **K0 first** (unblocks K2). K1 (bundle) is independent and the higher-value
  everyday win, so K0→K1→K2. Constellation's module/namespace work (its C4) is the shared
  foundation that makes multi-package builds meaningful — see `CONSTELLATION_PLAN.md`.
