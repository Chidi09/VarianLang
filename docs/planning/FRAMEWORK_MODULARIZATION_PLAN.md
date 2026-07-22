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
3. **Lumen Content & Security Leaves** (`vn_modules/lumen/content.vn`, `security.vn`):
   - Typed content collection discovery and live-channel origin policy are independently owned.
   - The facade retains ambient compatibility while mounts consume the extracted security policy.

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
| Server endpoints and middleware | Astro/SvelteKit routes and hooks | Zenith file routes, guards, middleware, typed request context | **done** — preserved request/context contract, onion/short-circuit middleware, normalized endpoint values, decoded params, HEAD/OPTIONS/405 semantics, contextual safe errors, lifecycle tests |
| Progressive form actions | SvelteKit actions/enhance | Native form fallback plus optional Lumen event transport | **done** — native named controls/action/method, zero-JS mode, typed server validation, automatic same-origin CSRF fields through `shield.csrf()`, structured field/form results, exact opt-in event action, lifecycle tests |
| Nested layouts and error boundaries | SvelteKit layouts/errors | Inherited `layout.lumen`, nearest `loading.lumen`, and nearest `error.lumen` with parent-layout retention | **done** — route collector/build/runtime tests |
| Data loading and invalidation | SvelteKit load/dependencies | Ordered layout/page loaders plus explicit stable dependency keys | **done** — outer-to-inner inherited data, deduplicated dependencies, keyed invalidation, and validated loader redirects that short-circuit descendants without client JS |
| Query cache | TanStack Query | Request-keyed server resource cache with stale windows, deduplication, previous-data retention, invalidation, and metrics | **done** — access-aware `gc_ms`, safe non-fetching eviction, version pruning, eviction metrics, focused cache tests |
| Mutations | TanStack Query/Form | Server mutation lifecycle with optimistic context, rollback/success callbacks, reset, and resource invalidation | **done** — blocking and cooperative background execution, immediate optimistic state, stale-completion generation guards, live notification, lifecycle tests |
| Router search state | TanStack Router | Schema-validated URL state and deterministic route matching | **done** — strict typed coercion, UTF-8 codec, validator/default contracts, URL marker and runtime tests |
| Tables/virtualization | TanStack Table/Virtual | Headless server-compatible models; optional measured browser action | **done** — server sort/filter/page, live DB refetch, bounded virtual windows, identifier contracts, opt-in `vtable` action and SQLite conformance tests |
| Deterministic browser actions | all three ecosystems | Registry plus dependency closure and exact emission | **done** — authoritative registry, detection markers, stable transitive dependency closure, exact module/byte tests |
| Server template rendering | Svelte/Astro templates | Escaped interpolation and server control flow | **done** — `zenith/template.vn`, focused tests |
| Streaming/deferred rendering | modern meta-frameworks | Ordered server chunks with explicit fallback boundaries | **done** — TLS-aware native chunks, zero-JS lazy ordered HTML, validated fallback/error boundaries, and one opt-in deduplicated replacement action with byte-budget coverage |

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

The scanner is driven by one action registry containing detection markers and explicit
dependencies. Resolution computes a stable transitive closure, emits each module once, and
tests assert exact module sets and byte ceilings.

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
