# Varian Website — Standard Build Plan

**For the implementing agent. The site lives in `website/` (Lumen `.lumen` pages + `public/`), served
by `vn dev website/pages <port>`. This plan makes it clean, minimal, content-rich, and HONEST (every
code sample is real Varian that runs; the landing console executes real code).**

> ⚠️ The current working tree was just restored from commit `f36a2c5`. A backup of the prior (damaged)
> tree is at `/tmp/website_damaged_backup`. Work from the restored files. Do NOT delete the user's pages.

---

## 0. Principles (non-negotiable)

1. **Clean & minimal, content-first.** Reference the restraint of the user's `naijaspride` project
   (`/root/dev/naijaspride`): Plus Jakarta Sans, generous whitespace, calm surfaces, subtle borders —
   not a candy-colored gradient soup.
2. **Typography:** **Plus Jakarta Sans** for everything (body + headings). **Remove Space Grotesk and
   Outfit entirely** — search every `.lumen`/`.css` for `Space Grotesk`/`Outfit` and replace. **JetBrains
   Mono** for code. **Material Symbols (Outlined)** from Google Fonts for all icons (open-source, free) —
   same kit naijaspride uses.
3. **Minimal hues, earned per page.** Global chrome is near-monochrome (neutral grays + one restrained
   accent for links/buttons). Color is *earned*, page by page — see §2.
4. **Every code sample is real and runnable.** All snippets come from `examples/` (verified) or are
   verified before shipping. The landing console **executes real Varian** via a backend endpoint — no
   hardcoded fake output (the current `runCode` fakes it; that is a bug to fix).
5. **The Varian logo (`/assets/varian-logo.png`) is in the nav on every page.** Today the nav uses a
   generic inline box-SVG, not the logo — fix it.

---

## 1. Global design system — `website/public/global.css` `:root`

Keep the existing token *names* (pages already use `var(--bg-main)` etc.); change values to a clean,
minimal neutral base and swap fonts.

```css
@import url('https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@300;400;500;600;700;800&family=JetBrains+Mono:wght@400;500;700&display=swap');
@import url('https://fonts.googleapis.com/css2?family=Material+Symbols+Outlined:opsz,wght,FILL,GRAD@20..48,100..700,0..1,-50..200');

:root {
  /* Neutral base — minimal, near-monochrome (zinc) */
  --bg-main: #09090b; --bg-surface: #131316; --bg-surface-hover: #1c1c20;
  --text-primary: #fafafa; --text-secondary: #a1a1aa; --text-muted: #71717a;
  --border-subtle: #232327; --border-strong: #34343a;

  /* ONE restrained global accent (links, primary buttons). Calm indigo, low saturation. */
  --accent: #818cf8; --accent-hover: #a5b4fc;

  /* Per-page theming injects --theme-1 / --theme-2 (see §2). Default = neutral. */
  --theme-1: var(--accent); --theme-2: var(--accent);

  --font-sans: 'Plus Jakarta Sans', system-ui, -apple-system, sans-serif;
  --font-display: 'Plus Jakarta Sans', system-ui, sans-serif;   /* same family, heavier weight */
  --font-mono: 'JetBrains Mono', ui-monospace, monospace;
  /* keep the existing --text-*, --space-*, --radius-* scales */
  --shadow-card: 0 1px 2px rgba(0,0,0,.3), 0 12px 28px -12px rgba(0,0,0,.5);
  --transition-fast: 150ms cubic-bezier(.4,0,.2,1);
}
```

**Borrow from naijaspride (treatments only, not its colors):**
```css
* { box-sizing: border-box; }
html { -webkit-font-smoothing: antialiased; scroll-behavior: smooth; }
body { font-family: var(--font-sans); background: var(--bg-main); color: var(--text-primary); line-height: 1.6; }
h1,h2,h3,h4 { font-family: var(--font-display); font-weight: 700; letter-spacing: -0.02em; line-height: 1.15; }
::selection { background: color-mix(in srgb, var(--theme-1) 30%, transparent); }
::-webkit-scrollbar { width: 10px; height: 10px; }
::-webkit-scrollbar-thumb { background: var(--border-strong); border-radius: 9999px; }
::-webkit-scrollbar-thumb:hover { background: var(--text-muted); }
:focus-visible { outline: none; box-shadow: 0 0 0 3px color-mix(in srgb, var(--theme-1) 45%, transparent); }
a { color: var(--accent); } a:hover { color: var(--accent-hover); }
.material-symbols-outlined { font-variation-settings: 'wght' 400, 'opsz' 24; vertical-align: middle; }
```

**Material Symbols usage:** `<span class="material-symbols-outlined">bolt</span>` — icon names:
`bolt` (speed), `network_node`/`hub` (concurrency), `shield` (safety), `deployed_code` (build/Kiln),
`package_2` (Constellation), `dns` (Zenith), `web` (Lumen), `auto_awesome` (Aurora), `terminal`,
`rocket_launch`, `bug_report`, `verified`, `code_blocks`, `download`.

> **Pin the base so the injected Lumen shell can't wash it out** (the `vn dev` shell injects
> `_lumen_head()` which ships its own `body{background:var(--lumen-bg)}`). Add, with high specificity:
> `html body { background-color: var(--bg-main); color: var(--text-primary); }`

---

## 2. Per-page theming (the only place hue is allowed)

Each page sets a theme class on its root wrapper; that class only re-defines `--theme-1/--theme-2`
(and, for Lumen, flips to light). Global neutral stays; the accent shifts subtly per product.

| Page | Theme | `--theme-1` / `--theme-2` | Notes |
|---|---|---|---|
| **Landing** + chrome | neutral | indigo `#818cf8` | minimal; almost monochrome |
| **Aurora** (`/aurora`) | **full hue** | aurora gradient `linear-gradient(135deg,#2EE6C5,#38BDF8,#6366F1,#C026D3)` | the ONLY hue-rich page; mirrors the logo; starfield-dark |
| **Lumen** (`/lumen`) | **bright/light** | accent `#f5b829` (amber) on **light** bg (`#FBFCFF`, text `#0B1020`) | the one light page |
| **Zenith** (`/zenith`) | normal | restrained `#5b9cff` (steel blue) | neutral dark, standard docs |
| **Kiln** (`/kiln`) | normal | ember `#f59e0b` used *sparingly* | neutral dark |
| **Constellation** | normal | violet `#a78bfa` sparingly | neutral dark |

Implement as `.theme-aurora`, `.theme-lumen-light`, `.theme-zenith`, etc. on each page's outermost
element, each block setting `--theme-1`/`--theme-2` (and Lumen overriding `--bg-main`/`--text-primary`).

---

## 3. Shared nav (every page) — WITH the Varian logo

Replace the inline box-SVG with the real logo image. Add `Aurora` to the links.

```html
<nav class="navbar">
  <a href="/" class="nav-brand">
    <img src="/assets/varian-logo.png" alt="Varian" width="28" height="28" style="border-radius:6px">
    <span>Varian</span>
  </a>
  <div class="nav-links">
    <a href="/#language">Language</a>
    <a href="/#ecosystem">Ecosystem</a>
    <a href="/aurora">Aurora</a>
    <a href="/zenith">Zenith</a>
    <a href="/lumen">Lumen</a>
    <a href="/the_varian_book">The Book</a>
    <a href="https://github.com/Chidi09/VarianLang">GitHub</a>
  </div>
</nav>
```
(`varian-logo.png` is large — 1.9 MB committed; downscale/optimize it to ~64px PNG or it'll bloat load.)

---

## 4. Landing page (`pages/index.lumen`) — section by section (make it RICH)

Varian is a brand-new language; the landing must *teach* it, not just hype it. Order + content:

### 4.1 Hero
- H1: **"Varian — one language for the whole stack."**
- Sub: "A fast, memory-safe language with native actors and channels, a batteries-included web
  framework (Zenith), a server-driven UI framework (Lumen), a build tool (Kiln), and a package
  registry (Constellation). Aurora is all of it, together."
- Two CTAs: `Get started` (→ #install), `Read the book` (→ /the_varian_book).
- Right/below: the **interactive console** (§5) pre-loaded with the Hello example.

### 4.2 Interactive console (REAL — see §5 for wiring)
Default snippet (real, runnable — replaces the current fake `make(chan)`/`spawn fn`):
```
// Native actors + channels, no thread overhead.
fn worker(ch) {
    ch <- "Hello from a Varian actor!"
}
let ch = task.channel(1)
task.spawn(worker, [ch])
print(<- ch)
```
A `Run ▶` button POSTs the buffer to `/api/run` and shows real stdout. Ship a few **example tabs**
the user can load (each verified runnable, §7): *Hello*, *Actors*, *Fibonacci*, *Struct*, *HTTP server*.

### 4.3 "Why Varian" — three short value cards (Material Symbols icons)
- `bolt` **Fast** — compiled bytecode VM; ~7,500 req/s single-process Zenith (link benchmarks).
- `hub` **Concurrent by default** — green-thread actors + channels, cooperative scheduler, one binary.
- `shield` **Safe** — memory-safe core, escape-by-default templating, friendly errors with line numbers.

### 4.4 Language tour — TEACH it (this is the "plenty of detail" the user wants)
A vertical sequence of `code + explanation` blocks. Every snippet is real & runnable (§7). Cover:

1. **Hello / values** — `print`, `let`, types are inferred.
   ```
   let name = "Varian"
   print("Hello, " + name)        // Hello, Varian
   ```
2. **Functions** — `fn`, returns, first-class & closures.
   ```
   fn add(a, b) { return a + b }
   print(add(2, 3))               // 5
   ```
3. **Structs** (use `examples/structs.vn`):
   ```
   struct Point { x: int, y: int }
   let p = Point { x: 10, y: 20 }
   print(p.x)                     // 10
   ```
4. **Control flow** — `if`/`else`, `for i in 0..n`, `while`.
5. **Actors** (use `examples/actor_hello.vn`):
   ```
   actor Greeter {
       name: string,
       fn greet(self) { print("Hello from actor!") }
   }
   let a = Greeter.spawn()
   a.greet()                      // Hello from actor!
   ```
6. **Channels** (real syntax — `examples/chan_test.vn`): `task.channel(n)`, `task.spawn(fn,[args])`,
   `ch <- v`, `<- ch`, `task.yield()`.
7. **Errors** — `try { ... } catch e { ... }` (use `examples/errors.vn`).
8. **A web server in 6 lines** (Zenith):
   ```
   let app = new_app()
   app.get("/", |req| {
       return Response { status: 200, body: "Hello, World!", content_type: "text/plain" }
   }, "Root", null)
   app.listen(8080)
   ```
   (Note: `app.get` takes 4 args — path, handler, description, schema.)

Each block: left = code (JetBrains Mono, syntax-highlighted via the existing `token-*` classes or
`/public/highlight.js`), right = 2–3 sentences. Make `Run ▶` available on the runnable ones too.

### 4.5 Features grid
6–8 `feature-card`s with Material Symbols: actors/channels, memory safety, batteries-included stdlib
(http/db/mail/queue/storage), Zenith web framework, Lumen UI, Kiln build, Constellation packages,
benchmarks. One sentence each, link to the relevant page.

### 4.6 Ecosystem — Aurora umbrella + 4 products (with logos)
Intro line: **"Aurora is the fullstack platform: Zenith (backend) + Lumen (frontend), built with Kiln,
shipped through Constellation."** Then a card grid (use the committed logos in `/assets/`):
- **Aurora** (`/aurora`, `auto_awesome`, `aurora-logo.png`) — featured/first, the umbrella.
- **Zenith** (`/zenith`, `zenith-logo.png`) — backend web framework.
- **Lumen** (`/lumen`, `lumen-logo.png`) — server-driven UI.
- **Kiln** (`/kiln`, `kiln-logo.png`) — `vn build`.
- **Constellation** (`/constellation`, `constellation-logo.png`) — `vn add`.

### 4.7 Benchmarks / comparison
Small honest table: Zenith ~7,500 req/s single-proc, ~35–38k 8-worker, vs Go ~85k, Rust ~113k (don't
inflate). Frame as "fast for a young dynamic language, getting faster."

### 4.8 Get started (`#install`)
```
# build from source
git clone https://github.com/Chidi09/VarianLang && cd VarianLang && make
# your first program
echo 'print("Hello, World!")' > hello.vn
./vn run hello.vn            # Hello, World!
# a new fullstack app
./vn new myapp && cd myapp && vn dev
```
Plus: `vn run`, `vn dev`, `vn build`, `vn test`, `vn fmt`, `vn lint`, `vn add` one-liners.

### 4.9 Community + footer (lots of links)
GitHub (repo, issues, PRs, releases), The Book, each product page, license, "built with Varian."

---

## 5. The runnable console — architecture (kills the fake output)

The console must run real code. Add a Zenith backend endpoint and have the page call it.

**Backend** — a small Zenith app (or fold into the site's server) exposing `POST /api/run`:
- Body: `{ "code": "<varian source>" }`.
- Writes code to a temp file, runs it in a **sandbox**: `timeout 5 ./vn run <tmp>` under
  `ulimit -v 262144` (256 MB) and a working dir with **no network, no FS write access** beyond tmp.
  (Reuse the project's existing sandbox patterns; never run untrusted code unsandboxed.)
- Returns `{ "ok": bool, "stdout": "...", "stderr": "..." , "ms": n }`. Cap output length.
- Rate-limit per IP (Zenith `shield.rate_limit`). Strip/deny `ffi`, `python`, `net`, fs writes,
  `task.spawn` bombs via the timeout+memcap.

**Frontend** (`runCode` handler in `index.lumen`): replace the fake body with a real call. Options:
- **LumenJS/`<client>`**: `fetch('/api/run', {method:'POST', body: JSON.stringify({code})})` then write
  `result.stdout` into the output pane. (Small, fits the "minimal JS" rule.)
- Show stderr in red, exit status, and elapsed ms. Loading state while running.

**Security is mandatory** — this executes arbitrary user code. Sandbox (timeout + `ulimit -v` + no net +
ephemeral tmp dir + output cap + rate limit) is non-optional. Document it in the handler.

---

## 6. Per-product pages (`pages/<product>.lumen`)

Keep the restored content; restyle to the per-page theme (§2) and ensure every example is runnable.

- **`/aurora`** — the only hue-rich page. Aurora-gradient hero (logo colors), starfield-dark. Content:
  what Aurora is (Zenith+Lumen union), `vn new`/`vn dev`/`vn build` workflow, a runnable fullstack
  example, links to Zenith & Lumen. Uses `aurora-logo.png`.
- **`/lumen`** — **bright/light page** (`.theme-lumen-light`). Server-driven UI explainer, a runnable
  counter/component example, the component vocabulary, light/dark note. `lumen-logo.png`.
- **`/zenith`** — neutral dark, steel-blue accent. The 6-line server (runnable), routing, middleware,
  db/auth/queue, benchmarks. `zenith-logo.png`.
- **`/kiln`**, **`/constellation`** — neutral, sparing accent. `vn build`/`vn add` flows, runnable-ish
  CLI transcripts. Respective logos.
- **`/the_varian_book`** — keep the 904-line book; just apply the new nav, fonts, and neutral theme.

Each product page: shared nav (with logo) + the per-page theme class + consistent footer.

---

## 7. Verified runnable example library (use these — they pass)

Source from `examples/` (all confirmed to run with `./vn run`). Expected output in comments:

| Snippet | Source | Output |
|---|---|---|
| `print("Hello, World!")` | hello.vn | `Hello, World!` |
| Fibonacci(10) | fib.vn | `55` |
| Struct `Point{x,y}` | structs.vn | `10` / `20` / `0` |
| Actor `Greeter` | actor_hello.vn | `Hello from actor!` / `done` |
| Channels `task.channel`/`<-` | chan_test.vn | `sent` / `42` / `100` / `received` |
| Enums | enums.vn | (runs) |
| Generics | generics.vn | (runs) |
| Errors `try/catch` | errors.vn | (runs) |
| HTTP server | (the 6-liner in §4.4) | serves :8080 |

**Rule:** before shipping ANY snippet on the site, run `./vn run <file>` and confirm exit 0 + the shown
output. Do NOT invent syntax. (The current landing uses `make(chan,10)`/`spawn fn(){}`/`send`/`recv`
and a `fn main(){}` wrapper — **all fake**: `spawn fn` is a parse error, `main()` is never auto-called,
and real concurrency is `task.channel`/`task.spawn`/`ch <- v`/`<- ch`. Replace it.)

---

## 8. Concrete fix-list (what's wrong today → do this)

1. Remove **Space Grotesk** and **Outfit** everywhere → Plus Jakarta Sans. (grep all `.lumen`/`.css`.)
2. Add **Material Symbols** import + use icons (no more hand-rolled SVGs except logos).
3. Put **`varian-logo.png`** in the nav (replace the inline box-SVG); downscale the asset.
4. **Reduce hues globally** to the neutral zinc base + one indigo accent; move all color into the
   per-page themes (§2). Aurora = hue, Lumen = light, others = neutral.
5. Make the **console real**: `/api/run` sandboxed endpoint + `runCode` fetches it; default + tab
   snippets all runnable.
6. Replace every **fake code sample** with verified `examples/` snippets (§7).
7. Pin `html body` background so the Lumen-shell injection can't wash the theme out.
8. Keep the restored content (book, all product pages, full nav, landing sections) — only restyle.

## 9. Order of work
global.css tokens/fonts/treatments → shared nav (logo) → per-page theme classes → landing sections
(4.1→4.9) → `/api/run` + real console → per-product restyle → replace all examples → verify each page
renders (`vn dev website/pages <port>`, curl each route 200, eyeball) and every snippet runs.

## 10. Reference
- Clean styling cues: `/root/dev/naijaspride` (Plus Jakarta Sans, Material Symbols, restraint).
- Real syntax: `examples/*.vn` (the source of truth — never guess Varian syntax).
- Brand assets: `website/public/assets/{varian,zenith,lumen,kiln,constellation}-logo.png`, `aurora-logo.png`.
