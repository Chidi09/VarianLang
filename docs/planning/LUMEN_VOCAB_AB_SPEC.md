# Lumen Vocabulary — Ship Plan for (A) Section Components + (B) Style-Prop System

**For the implementing agent. This is pure-Varian work in `vn_modules/lumen.vn` — NO C changes,
no parser changes, no native methods (so the method-dispatch gate does NOT apply).** Everything
plugs into the existing built-in vocabulary exactly as `<Stack>`/`<Card>`/`<Button>` already do.

## Ground rules (read once, obey throughout)

1. **Where things live, all in `vn_modules/lumen.vn`:**
   - Render fns next to the existing `_ui_render_*` (around lines 1803–1900).
   - All new CSS goes **inside `_lumen_design_css()`** (the single `<style id="lumen-ui">` block,
     ~line 1776), appended as more `.lmn-*` rules. Do **not** add new `<style>` tags or inline
     `<style>` per component — one stylesheet, emitted once by `<Page>`.
   - Register each new component with `lumen_register_component("Name", _ui_component(_ui_render_name))`
     in the registration list (~lines 1923–1937).
2. **Token-driven only.** Colors/space/radius/shadow come from the `--lumen-*` CSS variables and the
   `_ui_space()` / `_ui_font()` scales. Never hardcode hex or px that a token already covers. This is
   what makes light/dark/brand automatic — and it's the `--lumen-` prefix, NOT `--lmn-` (that typo bug
   is why earlier cards rendered unstyled; grep the repo for `--lmn-` and fix any stragglers).
3. **Variants = `data-*` attributes + CSS attribute selectors** (like `.lmn-btn[data-variant=primary]`).
   Continuous values (gap, font-size, an arbitrary width) = inline `style=`. Don't invent a class per value.
4. **Escape anything that can hold user/DB data.** Props like `image`, `title`, `href`, `label`, `alt`
   are interpolated into HTML/attributes by string concat — wrap them in `_sanitize.escape_html(...)`
   (already used in `_lumen_meta_tags`). The existing `_ui_render_button` does NOT escape `href` — fix
   that as you go. Enumerated tokens (`tone`, `size`, scale keys) don't need escaping.
5. **Stateless/presentational.** These are all `_ui_component` render fns with `| | { return {} }` state.
   Interactive behavior is the author's `on="handler"` → Varian, never JS. (Modal/Tabs/Accordion that
   need server state are explicitly OUT of this pass — see "Deferred" at the end.)
6. **Keep it quiet.** Ship the components and props listed here and stop. No 25-component dump.

---

## PART B — the shared style-prop vocabulary (build this FIRST; it multiplies A)

**Goal:** every component (existing and new) accepts a small, memorizable set of layout/spacing/surface
props, so authors compose UI without opening a `<style>` block. One scale, learnable in 30 seconds.

### B1. The prop vocabulary (final list — do not expand)

| Prop | Maps to | Values |
|---|---|---|
| `pad` / `padx` / `pady` | padding / padding-inline / padding-block | space scale (`_ui_space`) |
| `m` / `mx` / `my` / `mt` / `mb` | margin variants | space scale |
| `gap` | gap | space scale (flex/grid only; harmless elsewhere) |
| `align` | align-items | `start\|center\|end\|stretch` → flex values |
| `justify` | justify-content | `start\|center\|end\|between\|around` → flex values |
| `w` / `maxw` | width / max-width | `sm\|md\|lg\|xl\|full` (reuse the `.lmn-container` max-widths) |
| `bg` | background | `surface\|surface-2\|muted\|primary` → `var(--lumen-…)` |
| `tone` | color + background pair | `default\|muted\|primary\|success\|danger\|info` (token sets) |
| `radius` | border-radius | `none\|sm\|md\|lg\|full` (`0`/`8px`/`var(--lumen-radius)`/`999px`) |
| `shadow` | box-shadow | flag → `var(--lumen-shadow)` |
| `border` | 1px token border | flag → `1px solid var(--lumen-border)` |

`align`/`justify` take short words (`start`, `between`) and map to the CSS longhand — don't make authors
type `flex-start`/`space-between`.

### B2. One helper, threaded through every render fn (the clean part)

Add a single function that turns the props present on `s` into a CSS-declaration string:

```
// Resolve the shared style props on `s` into a CSS declaration string ("" if none).
// Append this to any component's own inline style so every built-in understands
// pad/gap/tone/radius/shadow/etc. without each render fn re-implementing them.
fn _ui_style_props(s) {
    let d = ""
    let pad = _validate.get_field(s, "pad");   if pad  != null { d = d + "padding:" + _ui_space(pad) + ";" }
    let px  = _validate.get_field(s, "padx");  if px   != null { d = d + "padding-inline:" + _ui_space(px) + ";" }
    let py  = _validate.get_field(s, "pady");  if py   != null { d = d + "padding-block:" + _ui_space(py) + ";" }
    let m   = _validate.get_field(s, "m");     if m    != null { d = d + "margin:" + _ui_space(m) + ";" }
    // ... mx/my/mt/mb, gap, align(_ui_justify_align), justify, w/maxw, bg, radius(_ui_radius),
    //     shadow(_ui_flag), border(_ui_flag), tone(_ui_tone) — same shape, all token-resolved ...
    return d
}
```

Add the tiny resolver helpers next to `_ui_space`/`_ui_font`: `_ui_radius(v)`, `_ui_align(v)`,
`_ui_justify(v)`, `_ui_bg(v)`, `_ui_tone(v)` (returns `color:…;background:…;`). Same enumerated-`if`
style as `_ui_space` so an unknown value degrades to a sane default, never errors.

**Thread it in with one mechanical edit per existing `_ui_render_*`.** Worked example —
`_ui_render_stack` today:

```
return "<div class=\"lmn-stack\" style=\"gap:" + gap + ";align-items:" + align + "\">" + _ui_slot(s) + "</div>"
```
becomes:
```
return "<div class=\"lmn-stack\" style=\"gap:" + gap + ";align-items:" + align + ";" + _ui_style_props(s) + "\">" + _ui_slot(s) + "</div>"
```

Apply the identical `+ _ui_style_props(s)` merge to the `style="..."` of every built-in
(`Container, Section, Stack, Row, Grid, Card, Heading, Text, Eyebrow, Button, Badge, Feature, Divider,
Spacer`). For components that currently emit no `style=` (e.g. `Eyebrow`, `Divider`), add
`style=\"" + _ui_style_props(s) + "\"` only when it's non-empty (cheap: `let sp = _ui_style_props(s); let st = ""; if sp != "" { st = " style=\"" + sp + "\"" }`). Author-set props always win because they're appended last.

### B3. Document the vocabulary in one place

Put the B1 table as a comment block above `_ui_style_props` so it's discoverable. That comment is the spec.

---

## PART A — the section components (build AFTER B, so they inherit the props)

Each is a stateless render fn + CSS rules in `_lumen_design_css()` + a registration line. All accept the
Part-B props automatically (call `_ui_style_props(s)` in their root style). Ship exactly these:

### A1. `<Hero eyebrow= title= subtitle= image= align=>`
Collapses the ~40-line hero block in `index.lumen`. Structure: a `.lmn-hero` grid (text | image),
collapses to one column under 760px; `align="center"` centers + drops the image column. Eyebrow→`.lmn-eyebrow`,
title→big `.lmn-h`, subtitle→muted `.lmn-text`, slot = the actions row (author drops `<Button>`s in).
`image` is optional and **escaped**. CSS: reuse `.lmn-h`/`.lmn-eyebrow`/`.lmn-text`; add `.lmn-hero`,
`.lmn-hero-img` (rounded, `object-fit:cover`, `var(--lumen-shadow)`).

### A2. `<Nav brand= links= actions=>` and `<Footer>`
`Nav`: sticky top bar, `.lmn-nav` flex row — brand (left), links (center/right), `actions` slot (right).
`links` may be the slot for flexibility. `Footer`: `.lmn-footer` with top border token, muted text, slot content.

### A3. `<Split ratio= gap= reverse=>`
Responsive two-column (the hero-inner pattern, reusable). `.lmn-split` grid, `ratio` default `1fr 1fr`,
collapses to single column under 760px. `reverse` flag swaps order on desktop. Slot = the two children.

### A4. `<Field label= error= hint= for=>`
Form row: label + slot (the input the author drops in) + optional hint + error. `.lmn-field` stack;
`.lmn-field-error` uses `--lumen-danger`. Label `for`/input wiring is the author's; this is layout+states.
**Escape `label`/`hint`/`error`.** This single component removes most form CSS.

### A5. Atoms: `<Stat value= label=>`, `<Tag tone=>`, `<Avatar src= alt= size=>`, `<Alert tone= title=>`,
`<Empty icon= title=>`, `<Skeleton w= h= radius=>`
- `Stat`: big `.lmn-h` number + muted label — KPI blocks.
- `Tag`: like `Badge` but slimmer; reuse `.lmn-badge` tone CSS or add `.lmn-tag`.
- `Avatar`: rounded `img` (escape `src`/`alt`), `size` from `_ui_space`/explicit; fallback initials from slot.
- `Alert`: `.lmn-alert[data-tone=…]` callout (icon via `lumen_icon` + title + slot), token bg per tone.
- `Empty`: centered empty-state (icon + title + slot) for "no results".
- `Skeleton`: `.lmn-skel` shimmer placeholder (CSS keyframe animation), `w`/`h`/`radius` props.

Use `lumen_icon()` for any icons (extend that set if a needed glyph is missing — same inline-SVG style).

---

## Verification (required before calling A/B done)

1. **Build/tests:** `make -j4` then `ulimit -v 9000000; ./vn test tests/` → 105/105 still green.
2. **New test `tests/lumen_vocab_test.vn`:** render a component using B props and the A components, assert:
   - `<Stack pad="6" gap="4" shadow>` emits `padding:1.5rem`, `gap:1rem`, `box-shadow:var(--lumen-shadow)` in the style.
   - `<Hero title="X" image="a\"b">` escapes the image attr (no raw `"` breaks the tag).
   - `<Alert tone="danger">` carries `data-tone="danger"` and the danger token CSS exists in the page.
   - A page using only built-ins emits the `lumen-ui` stylesheet exactly once.
3. **Rewrite `aurora/pages/index.lumen`'s hero with `<Hero>` + Part-B props and DELETE the `.aurora-hero`
   hand-CSS** — proof the vocabulary actually replaces hand-written HTML/CSS. The page must look the same.

## Sequencing

B1→B2→B3 (verify) → A1 → A2 → A3 → A4 → A5 (verify each). Commit per part. Stop at the line above.

## Deferred (do NOT build in this pass)

`<Modal>`, `<Tabs>`, `<Accordion>` — these need server-driven state via `on=` and the `/live` loop. They
belong in a later pass once the live path is re-confirmed (the add-to-cart heap-corruption fix in
`escape_promote` just landed; verify it under ASan before adding new live-stateful components).
