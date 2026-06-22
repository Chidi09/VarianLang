# Toolchain Catch-Up Plan — Linter, Formatter, Tester + Aurora branding for Kiln/Constellation

**For the implementing agent.** Five workstreams; do them in the order below (each ships and verifies on
its own). The throughline: the linter, formatter, and tester were last updated before
Lumen's `.lumen` SFCs, the built-in UI vocabulary, the style-prop system, and the **Aurora** fullstack
identity existed — they're now blind to most of what people actually write. And Kiln (`vn build`) +
Constellation (`vn add`) still speak "Lumen", not "Aurora" (the name for the Zenith+Lumen union).

Honor [[feedback-no-half-measures]] (fix at the root) and [[project-method-dispatch-gate]] (the native
list is a real source of silent bugs). Aurora naming decided 2026-06-22 — see [[project-lumen-frontend]].

## Current state (verified, so you extend rather than rewrite)

- `src/lint.c`: AST walker. Has `shadowed`, `unused`, SQL-injection (`check_sql_call_unsafe` on
  `sqlite/postgres.query` string-concat), missing-`LIMIT` (`check_sql_args_missing_limit`), and a
  `known_native_methods[]` / `known_decorator_names[]` allowlist. Operates on `.vn` ASTs only.
- `src/fmt.c`: token-based `.vn` formatter (`fmt_scan_token`, etc.). No `.lumen` awareness.
- `src/test_runner.c`: `test_run_dir` → `collect_test_files` → `run_test_file`; compiles with modules,
  runs `compiler.tests[]`, supports `--filter` and `timeout_ms`.
- `src/main.c`: `lumen_new()` (line 629) scaffolds `pages/index.lumen` + components, prints "Lumen".
  Kiln auto-enters Lumen mode when `pages/` exists. `src/pkg_manager.c`: Constellation (`constellation.toml`).

---

## Workstream 1 — Linter catches up to Lumen + Aurora

Add an `.lumen`-aware pass plus rules for the footguns this codebase has actually hit. Keep every
existing rule.

1. **`.lumen` linting.** Teach `vn lint` to accept `.lumen` files: split the SFC into
   `<template>`/`<style>`/`<script>`, lint the `<script>` block as Varian through the existing AST path
   (offset line numbers so reported locations map to the real file), and run template/style text rules
   (below) on the other two blocks.
2. **Sync the dispatch-gate allowlist.** `known_native_methods[]` is stale relative to the methods added
   this cycle (struct `set/get/has/keys`, the lib_* additions, Lumen natives). Re-derive it from the
   actual `vm_register_dispatch(...)` calls across `src/lib_*.c` — ideally generate it at build time from
   a grep so it can't drift again. A stale entry here = the exact silent-wrong-value bug in
   [[project-method-dispatch-gate]].
3. **New Lumen/Aurora rules (warn, with `file:line`):**
   - **`--lmn-` token typo** — any `--lmn-*` in a `<style>`/style attr is almost certainly meant to be
     `--lumen-*` (this caused real unstyled cards). High-signal, near-zero false positives.
   - **Unescaped interpolation in HTML-attribute context** — a `{{ expr }}` placed inside an attribute
     (`src="{{ x }}"`, `href`, `title`, `alt`) where `expr` is not a literal/enumerated token → warn to
     route it through `_sanitize.escape_html`. XSS surface. (Reuse the escaping convention from
     `_lumen_meta_tags`.)
   - **Hardcoded color where a token exists** — raw `#rrggbb`/`rgb(...)` in a `.lumen` `<style>` that maps
     to an existing `--lumen-*` token → suggest the token (keeps light/dark/brand automatic).
   - **`on="handler"` with no matching `fn handler`** in the same component `<script>` → warn (dead event).
   - **Unknown component tag** — a `<PascalCase>` tag that is neither a registered built-in
     (Page/Stack/…/the new Hero/Nav/… set) nor a local/imported component → warn (catches typos &
     missing imports at lint time instead of a blank render).
   - **Client-JS-where-Varian-suffices** — a `<client>` block whose body only does a `fetch`/DOM toggle
     that an `on=` server handler could do → advisory note (ties to the LumenJS philosophy in
     `docs/planning/LUMEN_CLIENT_JS_SPEC.md`; keep it advisory, not an error).
4. **Verify:** add `.lumen` fixtures under `tests/lint/` (one clean, one tripping each rule); a clean
   Aurora page must lint silently; assert each bad fixture emits exactly its expected category.

## Workstream 2 — Formatter understands `.lumen` SFCs

`vn fmt` must format `.lumen` without mangling it.

1. **SFC split + dispatch.** Detect `.lumen`, split into the three blocks, and:
   - `<script>` → format with the existing `.vn` engine, re-indented one level inside the block.
   - `<style>` → minimal, safe CSS tidy (consistent indent, one rule per line, trim) — do **not** rewrite
     selectors/values.
   - `<template>` → HTML/JSX-ish reflow: consistent 2-space indent for nested tags, preserve `{{ }}`,
     `{{#each}}`/`{{/each}}`, `@event`/`on=` and self-closing `<Comp/>` verbatim. When in doubt, preserve.
2. **Block order + idempotency.** Canonical order `template → style → script` (or whatever the existing
   examples use — match, don't impose). Hard requirement: **`fmt(fmt(x)) == fmt(x)`** for every
   `tests/**/*.lumen` and every `vn_modules`/`aurora` page. Add an idempotency test that runs fmt twice.
3. **Never corrupt on parse failure.** If a block doesn't parse, leave that block byte-for-byte unchanged
   and format the rest. Formatter must be safe on half-written files (it runs on save in `vn dev`).
4. **Verify:** `vn fmt` every `.lumen` in the repo, confirm `git diff` is sane and re-running is a no-op.

## Workstream 3 — Tester matches the current system

Add first-class helpers so Lumen/Aurora behavior is testable, not just pure functions.

1. **Lumen render assertions.** A test helper to render a component to HTML and assert on it:
   `lumen_test_render(Comp, state)` → HTML string; plus matchers (`contains`, `has_attr`,
   `renders_once` for the `lumen-ui` stylesheet). Lets a test assert `<Hero title="X">` produces the
   eyebrow/title/actions structure, that props from Workstream B resolve, and that escaping happened.
2. **Server-driven event assertions.** Simulate the `on=`→handler→re-render loop **without a socket**:
   `lumen_test_event(Comp, state, "handler", value)` → `{ state, html }`, exercising the same child-state
   path that just had the `escape_promote` double-free. This is the regression test that path never had —
   add one that fires N events across a `{{#each}}` of components and asserts no crash + correct counts
   (the add-to-cart scenario, in-process).
3. **Zenith/Aurora route harness.** Build on the D3 HTTP tester helpers: spin an app in-process, issue
   `GET`/`POST` against routes, assert status/body/headers — no real port. Provide an Aurora smoke test
   that boots the scaffolded app and hits `/`, `/shop`, an `on=` event, and the API.
4. **Verify:** the new helpers are themselves covered; `ulimit -v 9000000; ./vn test tests/` stays green
   and now includes a live-path regression test.

## Workstream 4 — Kiln + Constellation speak "Aurora" ("different shi")

Aurora = the fullstack platform (Zenith backend + Lumen frontend together). The toolchain should
scaffold, build, and describe **Aurora apps**, not just "Lumen" frontends.

1. **Scaffolder → full Aurora app.** Promote `vn lumen new` to **`vn new <app>`** (keep `lumen new` as an
   alias) that scaffolds the *whole* stack, mirroring the reference app's structure (the `aurora/` demo,
   which must itself be **renamed** to free the platform name — e.g. `examples/storefront/`): `main.vn`
   (`new_app()` + Lumen `pages/` mount + `lib/` split + config), `pages/`, `lib/`, `public/`,
   `constellation.toml`. Banner/printed next-steps say **Aurora**, not Lumen.
2. **Kiln branding + detection.** `vn build` output and the dev console should identify an **Aurora app**
   (detect = `new_app()` usage *and* `pages/` present → "Aurora"; `pages/` only → "Lumen"; server only →
   "Zenith"). Update banners in `src/main.c` (`lumen_print_banner` / the `vn dev` console) and Kiln's build
   messages accordingly. Keep the LUMEN chip for pure-frontend; add an AURORA chip for fullstack.
3. **Constellation manifest.** `constellation.toml` gains an optional `kind = "aurora" | "zenith" | "lumen"`
   (default inferred as in #2) so `vn add`/`vn build` and future registry tooling know the project shape.
   `vn publish`/`search` copy that says "Aurora package" where appropriate.
4. **Docs + naming sweep.** Update `docs/KILN.md`, `docs/CONSTELLATION.md`, help text (`src/main.c` usage),
   and scaffolded READMEs to introduce **Aurora** as the umbrella, with Zenith/Lumen as its two halves.
   Grep for user-facing "Lumen app"/"Zenith app" strings and reclassify the fullstack ones to "Aurora."
5. **Verify:** `vn new demo && cd demo && vn build` produces a runnable Aurora app; banners say Aurora;
   `vn build` on a server-only project still says Zenith; on a `pages/`-only project still says Lumen.

---

## Workstream 5 — Website (`website/`): Aurora rebrand + restyle (the showcase)

`website/` is itself an Aurora app (`.lumen` pages + `public/`). It currently brands "Varian/Lumen",
ships the old Lumen favicons, and is typed in **Outfit + Space Grotesk** with a grayscale palette
(`website/public/global.css`). It must (a) become **Aurora**, (b) wear the new Aurora identity, (c)
borrow refined typographic treatments from the user's other work, and (d) **dogfood the A/B vocabulary +
LumenJS** — a framework's own site should be the proof it works. Add **more than enough** detail; this is
the public face. Hard constraint from the user: **do NOT copy naijaspride's color palette** (its "cinema
old-money" burgundy/`--cinema-*`) — Aurora's colors come from its **own logo**.

### 5.1 Install the Aurora favicon + logo assets (concrete file ops)
- Source favicon set: **`docs/favicons (10).zip`** (7 files: `favicon.ico`, `favicon-16x16.png`,
  `favicon-32x32.png`, `apple-touch-icon.png`, `android-chrome-192x192.png`, `android-chrome-512x512.png`,
  `manifest.json`). Unzip and **replace** the matching files in `website/public/` (the old Lumen set there
  now is stale).
- Source brand mark: **`docs/92b2a8c9-5125-4a23-b38d-233cdadbd57c-removebg-preview.png`** — the Aurora "A"
  (aurora-borealis gradient A with a starfield night-sky in the negative space, transparent bg). Copy to
  `website/public/aurora-logo.png` and use it as the site logo (replace `lumen-logo.png` usage in the nav/hero).
- Propagate to the scaffold so every `vn new` app ships Aurora branding: copy the same favicon set into
  **`vn_modules/lumen_assets/`** (the batteries-included starter assets scaffolded to `public/` — see
  `_lumen_head()` and `_lumen_build_dir`). Reconcile the zip's `manifest.json` `theme_color`/`background_color`
  with Aurora's night-sky base (5.3), and update the `theme-color` `<meta>` defaults in `_lumen_head()`
  (`vn_modules/lumen.vn` ~line 1684) from the current `#ffffff`/`#0a0e16` to the Aurora light/dark bases.
- Verify each page's `<head>` references the new icons and the manifest validates (no 404s — the prior
  crash log showed `/manifest.json` and favicons 404'ing; confirm `serve_static("/","public")` covers them).

### 5.2 Aurora color tokens — DERIVED FROM THE LOGO (not naijaspride)
Sample the logo PNG and build a token system around the aurora gradient on a night-sky base. Suggested
starting values (agent should sample the actual PNG to refine, then lock these into `global.css` `:root`
and the Lumen `--lumen-*` set so the site and the framework agree):
- **Night-sky base (dark):** `--bg: #070B18` / surface `#0E1426` / border `#1E263F`.
- **Aurora gradient (signature, used sparingly — wordmark, hero accent, focus glows):**
  `linear-gradient(135deg, #2EE6C5 0%, #38BDF8 30%, #6366F1 60%, #C026D3 100%)` (teal → cyan → indigo → magenta).
- **Primary solid (buttons/links):** the indigo/violet `#6366F1` (hover `#818CF8`); secondary teal `#2EE6C5`.
- **Light mode:** near-white bg (`#FBFCFF`), ink text (`#0B1020`), aurora gradient used only as accent.
- Provide BOTH light and dark; the logo is dark-native, so make **dark the default** (it shows the
  starfield best) with a light toggle. Never hardcode these hex outside the token block.

### 5.3 Typography — borrow from naijaspride, drop the overused face
The user explicitly likes **Plus Jakarta Sans** and noted **Space Grotesk is "very common"** — so move off it.
- **Body/UI:** `Plus Jakarta Sans` (300–700) — borrowed from naijaspride (`apps/web/src/styles.scss`).
- **Display/headings:** pick something **more distinctive than Space Grotesk** — recommend **Clash Display**
  or **General Sans** (Fontshare, free) or the **Degular** kit Lumen already licenses (Adobe Typekit
  `jsj7zpl`). Do NOT keep Space Grotesk as display.
- **Mono:** keep `JetBrains Mono`.
- **Treatments to borrow (NOT colors)** from naijaspride's `styles.scss`: heading `letter-spacing` (tracking
  ~`-0.02em` for display, the editorial `tracking-wide` feel), `-webkit-font-smoothing:antialiased`, a custom
  `::selection`, a thin rounded custom scrollbar (`::-webkit-scrollbar` ~8px, `border-radius:full` thumb),
  a `box-shadow: 0 0 0 4px <aurora-ring>` focus ring (recolored to Aurora), and `transition: … 0.2s ease-in-out`
  / a soft `cubic-bezier` for hovers. These give the "crafted" feel without touching the cinema palette.
- Update `global.css` `--font-sans`/`--font-display` and the Google Fonts `@import` accordingly; mirror the
  family choices into Lumen's `--lumen-font` so scaffolded apps match the site.

### 5.4 Dogfood the new vocabulary (A/B + LumenJS)
Rebuild the site's pages on the components shipped in `docs/planning/LUMEN_VOCAB_AB_SPEC.md`:
- Replace hand-rolled hero/nav/footer markup with `<Hero>`, `<Nav>`, `<Footer>`, `<Split>`, and the
  Part-B style props (`pad`/`gap`/`tone`/`radius`/`shadow`) — delete the bespoke per-page CSS that those
  now cover. The homepage especially should be near-zero `<style>`.
- Any interactivity (theme toggle, copy-code buttons, mobile nav) goes through **LumenJS** modules
  (`persist`, `clipboard`, `toggle`) per `docs/planning/LUMEN_CLIENT_JS_SPEC.md`, tree-shaken — not ad-hoc
  `<script>`. The site is the canonical example of "Varian does the logic, minimal JS."

### 5.5 Reframe the content as Aurora (umbrella) over Zenith + Lumen (halves)
- `pages/index.lumen` → an **Aurora** landing: the platform, with Zenith (backend) and Lumen (frontend) as its
  two halves, Kiln/Constellation as the toolchain. New Aurora logo + tagline.
- `pages/zenith.lumen`, `pages/lumen.lumen`, `pages/kiln.lumen`, `pages/constellation.lumen` → keep, but reframed
  as "part of Aurora"; consistent nav/footer; cross-link.
- `pages/the-varian-book.lumen` + `book-*.lumen` → header/nav rebrand only.
- Sweep all user-facing copy: "Built with Lumen" → "Built with Aurora" where it means the fullstack platform.

### 5.6 Website verification
- `cd website && vn build` (or the project's build cmd) → every page renders, returns 200, shows the Aurora
  favicon + logo, no 404 on icons/manifest.
- `vn fmt website/pages/*.lumen` is a no-op after formatting; `vn lint` clean (zero `--lmn-` typos, no
  unescaped interpolation) — this exercises Workstreams 1 & 2 on real pages.
- Dark is default and shows the starfield; light toggle works (LumenJS `persist`).
- Confirm the site is detected as an **Aurora** app by Workstream 4's heuristic.
- Capture before/after screenshots of the homepage for the user.

## Sequencing & guardrails

- Order: **1 (lint) → 2 (fmt) → 3 (test) → 4 (Aurora branding) → 5 (website).** 1–3 are additive and
  low-risk; 4 touches scaffolding/branding + the demo rename; 5 (website) comes last because it **depends on**
  A/B vocabulary, LumenJS, the Aurora tokens, and Workstream 4's detection all being in place — it's the
  integration showcase. The favicon/asset install (5.1) is the one piece that can land early and standalone.
- Each workstream: commit per workstream, add tests, and keep `ulimit -v 9000000; ./vn test tests/`
  green throughout (the standing memory-cap rule — uncapped runs have OOM'd the box).
- Single source of truth: the dispatch-gate native list (Workstream 1.2) should be generated, not
  hand-maintained, so lint/fmt/test/runtime never disagree about what methods exist again.
- Do **not** fold these into the A/B vocabulary work or the LumenJS work — separate PRs. This plan is the
  "different shi": the toolchain catching up to the framework, plus the Aurora rebrand of build/registry.
