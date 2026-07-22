# Framework Modularization Architecture Plan & Audit

This document outlines the architecture plan, package loader audit, and conceptual capability matrix for the modularization of **Lumen** (`vn_modules/lumen.vn`) and **Zenith** (`vn_modules/zenith.vn`) in Varian.

---

## 1. Executive Summary & Monolith Modularization Strategy

### 1.1 Objectives
The Varian web framework stack historically housed monoliths in `vn_modules/lumen.vn` (~213KB) and `vn_modules/zenith.vn` (~180KB). To improve maintainability and create clean subsystem ownership boundaries, we are conservatively extracting modular subsystems into Constellation package directories:
- `vn_modules/lumen/`
- `vn_modules/zenith/`

### 1.2 Phase 1 Extractions (Executed)
To ensure zero risk to existing application code and compiler semantics, Phase 1 executes **one leaf extraction per framework**:
1. **Lumen Browser Directives Catalog** (`vn_modules/lumen/browser_directives.vn`):
   - Extracted pure helper `_lumen_module(name)`.
   - Loaded in `vn_modules/lumen.vn` via `use "lumen"`.
2. **Zenith Template Engine** (`vn_modules/zenith/template.vn`):
   - Extracted template parser and evaluator (`_tpl_lookup`, `_tpl_truthy`, `_tpl_bind`, `_zenith_substr_index`, `_tpl_next_tag`, `_tpl_render_region`, `_tpl_keyword`, `_tpl_is_stop`, `_tpl_do_if`, `_tpl_do_for`, `render`, `render_response`).
   - Loaded in `vn_modules/zenith.vn` via `use "zenith"`.

---

## 2. Technical Audit of Varian Package Loader & Constellation System

### 2.1 Package Loader Mechanics (`src/parser.c`)
Varian package loading via `use "<name>"` uses `parser_resolve_use`:
1. Checks for direct file path (`<name>`).
2. Checks `./vn_modules/<name>/` package directory.
3. Checks `$VARIAN_HOME/vn_modules/<name>/` package directory for non-repository execution environments.

### 2.2 Directory Concatenation & Alphabetical Load Order
`parser_read_package_dir` in `src/parser.c` opens a package directory and reads all `.vn` files:
- Files are sorted alphabetically using `strcmp(names[i], names[j]) > 0`.
- All `.vn` files in the package directory are concatenated into a single parse buffer, separated by newlines.
- **Rule**: Sub-package files must either be independent of load order or named such that declarations precede usages according to ASCII alphabetical ordering.

### 2.3 Symbol Collision & Scope Checks
When a package is loaded via `use`:
- `src/parser.c` checks top-level functions defined by the package (`fn_before` to `parser->function_count`).
- If an un-aliased package defines a top-level function symbol that already exists in scope (from stdlib, top-level prelude, or previously loaded packages), `parser_error` is raised.
- **Supervising Repair Note**: Zenith's template engine contained a private string search helper named `_substr_index`. `vn_modules/mail.vn` (which loads earlier alphabetically in the top-level ambient prelude `vn_modules/*.vn`) also defined `_substr_index`. When `vn_modules/zenith.vn` issued `use "zenith"`, `parser.c` caught the collision. It was renamed to `_zenith_substr_index` in `vn_modules/zenith/template.vn`.

### 2.4 Ambient Facade & Dependency DAG
- **Top-level prelude**: Varian automatically loads `vn_modules/*.vn` (only top-level files) into the ambient prelude.
- **Package directories**: Subdirectories like `vn_modules/lumen/` and `vn_modules/zenith/` are ignored by ambient prelude globbing and loaded strictly on-demand via `use`.
- **Ambient Facade Pattern**: `vn_modules/lumen.vn` and `vn_modules/zenith.vn` remain in `vn_modules/*.vn` as facades. They issue `use "lumen"` and `use "zenith"`, making all extracted public APIs ambiently available without breaking backwards compatibility.
- **Current cost**: the facades are ambient, so both package directories are still parsed for every normal program. This phase does **not** reduce prelude parse time or runtime payload size. Parse-time gains require a later explicit-import prelude design with compatibility migration.

### 2.5 Dependency and ownership rules

The allowed direction is `facade -> package leaf -> existing lower-level prelude APIs`. Package leaves must not import their facade or another framework leaf. Lumen owns browser behavior discovery and emission; Zenith owns HTTP/application behavior and server rendering. Shared primitives belong in a separately named lower layer, not duplicated private helpers.

Because a package directory is one alphabetically concatenated parse unit, future files use ordered, responsibility-based names (`10_core.vn`, `20_actions.vn`) only where declaration order matters. A new file is not independently tree-shaken: every `.vn` file in that directory loads together.

---

## 3. Web Framework Capability Matrix: Modern Concepts Translated to Varian

Status is deliberately explicit: **done** means shipped and covered, **partial** means a useful subset exists, and **gap** means planned rather than parity claimed.

| Capability | Reference concept | Varian-native direction | Status / evidence |
| :--- | :--- | :--- | :--- |
| Zero optional JS | Astro static-first output | Server HTML plus directive-scanned micro-modules | **done** — `_lumen_scan_directives`, module tests |
| Visibility/idle/media activation | Astro island directives | Declarative activation policy for client blocks | **done** — `<client when="load|idle|visible|media">`, independent multi-island compilation, focused exact-emission tests |
| Content collections | Astro content schemas | Typed build-time content sources with deterministic manifests | **done** — `lumen/content.vn`, recursive ordering, safe Markdown, JSON/frontmatter schema tests |
| Server endpoints and middleware | Astro/SvelteKit routes and hooks | Zenith file routes, guards, middleware, typed request context | **partial** — routing exists; lifecycle/error contracts need conformance coverage |
| Progressive form actions | SvelteKit actions/enhance | Native form fallback plus optional Lumen event transport | **partial** — transport exists; validation/result protocol and optimistic rollback remain gaps |
| Nested layouts and error boundaries | SvelteKit layouts/errors | Inherited `layout.lumen`, nearest `loading.lumen`, and nearest `error.lumen` with parent-layout retention | **done** — route collector/build/runtime tests |
| Data loading and invalidation | SvelteKit load/dependencies | Page `load(req)` merges server data; keyed resources declare invalidation keys | **partial** — initial load and resource invalidation ship; dependency-key aggregation across layouts remains planned |
| Query cache | TanStack Query | Request-keyed server resource cache with stale windows, deduplication, previous-data retention, invalidation, and metrics | **done** — `lumen_cached_resource`, focused cache tests; expiry/garbage collection remains planned |
| Mutations | TanStack Query/Form | Server mutation lifecycle with optimistic context, rollback/success callbacks, reset, and resource invalidation | **done** — `lumen_mutation`, focused lifecycle tests; background mutation execution remains planned |
| Router search state | TanStack Router | Schema-validated URL state and deterministic route matching | **gap** |
| Tables/virtualization | TanStack Table/Virtual | Headless server-compatible models; optional measured browser action | **gap** |
| Deterministic browser actions | all three ecosystems | Registry plus dependency closure and exact emission | **partial** — catalog/scanner ship; explicit dependency metadata is the next milestone |
| Server template rendering | Svelte/Astro templates | Escaped interpolation and server control flow | **done** — `zenith/template.vn`, focused tests |
| Streaming/deferred rendering | modern meta-frameworks | Ordered server chunks with explicit fallback boundaries | **gap** |

---

## 4. Per-Browser-Action JS Registry & Budget Specifications

### 4.1 Directive Registry
Every browser directive maps statically from a `data-lumen-*` attribute to a pure JS snippet:
- `event-bridge`: Reconnect-safe WebSocket event push queue.
- `clipboard`: Modern `navigator.clipboard` wrapper with selector resolution.
- `focus`: Auto-focusing element helper.
- `scroll`: Smooth scrolling, overflow lock, and scroll position restoration.
- `toast`: Non-intrusive notification overlay.
- `key`: Global keyboard shortcut handler.
- `persist`: LocalStorage / SessionStorage input synchronizer.
- `time`: Relative time formatting and countdown tick timer.
- `media`: Lazy-loading media and video lightbox dialog.
- `window`: Online/offline, dark mode, and window size CSS variable synchronizer.
- `nav`: SPA link navigation intercepter with pushState.
- `toggle`: Dynamic element hidden state toggler.
- `transition`: CSS entry/exit animation triggers.
- `anchor`: Floating element positioning overlay.
- `inview`: IntersectionObserver event bridge.
- `sortable`: HTML5 drag-and-drop list reordering handler.

### 4.2 Performance & Quality Budgets
1. **Core Client Shell**: transport is 3,315 uncompressed bytes; the separately measured branded error presenter brings the mandatory total below a 4,100-byte regression ceiling, down from 6,945. Event capture and virtual-table behavior are no longer mandatory. The remaining target is below 2 KB after transport, reconciliation, and error presentation can be policy-separated without weakening passive server-push pages.
2. **Individual Directive Snippet**: ordinary actions have an 850-byte ceiling. The `events` action temporarily has a 1,800-byte ceiling because it owns form serialization and event modifiers; it is shipped only on interactive pages. Larger actions should be decomposed when that reduces real page payload rather than merely moving text.
3. **Zero Unused Bytes**: Pages with zero directives ship 0 bytes of optional directive JS.
4. **Deterministic Output**: Pure function output for identical input (`_lumen_module`).

The current scanner manually couples attribute names, module names, and the event-bridge dependency. The next phase replaces that coupling with one action registry containing detection markers and dependencies. Resolution must compute a stable transitive closure, emit each module once, and make tests assert exact module sets and byte counts—not merely search for a representative string.

---

## 5. Incremental migration phases

1. **Leaf extraction (complete here)**: one behavior-preserving leaf per framework, facades unchanged.
2. **Action registry**: centralize Lumen detection/dependency metadata and enforce exact output budgets.
3. **Framework seams**: extract routing, forms, resources, and response helpers one coherent subsystem at a time, with public facade compatibility tests.
4. **Opt-in prelude**: design explicit framework imports before claiming compiler parse-time improvements; retain a documented compatibility mode.
5. **Conformance applications**: static content, authenticated CRUD, streaming UI, and large data examples exercised in real browsers.

## 6. Verification & Quality Gates

The modularization is validated via the test suite:
- `tests/lumen_directive_package_test.vn`: Validates package boundary, stability, and catalog scanner parity.
- `tests/zenith_template_test.vn`: Validates template interpolation, control flow, raw rendering, and response wrapping.
- `tests/lumen_modules_test.vn`: Validates directive scanning and tree-shaking.
- `./vn test tests/`: Full regression suite execution across all system tests.
