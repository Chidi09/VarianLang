# Website Polish Plan — Ambient Video, Per-Framework Hues, Icon Cleanup, World-Class Feel

This is the "make it look hand-built, not AI-generated" pass. It assumes the site
already builds and serves (see `WEBSITE_PLAN.md` for the structural/content baseline).
Everything here is **visual polish + the ambient background video**. Do not regress
content, light/dark mode, nav links, or runnable examples.

Golden rules for this pass:
- **Restraint over decoration.** AI-looking sites are busy: too many colors, too many
  icons all the same color, cramped text, gradients on everything. We go the other way —
  one ambient motion layer, hues used *sparingly and meaningfully* (one per framework),
  generous whitespace, large readable type.
- **Every hue must mean something.** A color appears because it identifies a framework,
  not for fun. Aurora = cyan-blue, Zenith = blue, Lumen = amber/bright, Kiln = orange,
  Constellation = violet. Never mix two framework hues in one component.
- **Readability is non-negotiable.** Body text never below 16px. Line length capped ~70ch.
  Over the video, text gets *brighter and heavier*, never dimmer.

---

## 0. The ambient background video (DONE — assets already produced)

The source `16-9.mp4` (1280×720, 7.3s loop, abstract blue→indigo gradient) had a
`jitter.video` watermark bottom-right. It has been **zoom-cropped out** (gradient, so
zero visible loss) and re-encoded for web. These files now exist in `website/public/`:

- `aurora-bg.webm` — 222 KB, VP9, primary source
- `aurora-bg.mp4`  — 519 KB, H.264, Safari/iOS fallback
- `aurora-bg.jpg`  — 19 KB, poster frame (instant paint + `prefers-reduced-motion` still)

If you ever need to regenerate from `16-9.mp4`:
```
ffmpeg -i 16-9.mp4 -vf "crop=in_w-180:in_h-70:0:0,scale=1280:720" -an -c:v libvpx-vp9 -b:v 0 -crf 38 -row-mt 1 website/public/aurora-bg.webm
ffmpeg -i 16-9.mp4 -vf "crop=in_w-180:in_h-70:0:0,scale=1280:720" -an -movflags +faststart -pix_fmt yuv420p -crf 30 -preset slow website/public/aurora-bg.mp4
ffmpeg -ss 2 -i 16-9.mp4 -vf "crop=in_w-180:in_h-70:0:0,scale=1280:720" -vframes 1 -q:v 4 website/public/aurora-bg.jpg
```

### 0.1 How it should appear (this is the whole "subtle everywhere, never invading vision" requirement)

The video is **one fixed full-viewport layer behind everything**, NOT a per-section element.
It sits at `z-index:-1`, `position:fixed`, covers the viewport, and is heavily dimmed so
it reads as a faint living gradient — like the page is floating on water, not a video playing.

```html
<!-- First child of <body>, before .container -->
<div class="ambient-bg" aria-hidden="true">
  <video autoplay muted loop playsinline poster="/aurora-bg.jpg"
         preload="metadata">
    <source src="/aurora-bg.webm" type="video/webm">
    <source src="/aurora-bg.mp4" type="video/mp4">
  </video>
</div>
```

```css
.ambient-bg {
  position: fixed;
  inset: 0;
  z-index: -1;
  overflow: hidden;
  pointer-events: none;
}
.ambient-bg video {
  width: 100%;
  height: 100%;
  object-fit: cover;
  /* The key to "subtle, never invading vision": dim + desaturate + blur it. */
  opacity: 0.16;            /* dark theme: barely-there */
  filter: blur(60px) saturate(1.2);
  transform: scale(1.1);    /* hide blur edge bleed */
}
/* Light theme: the bright gradient would wash out text — pin it lower + lighten blend */
.theme-light .ambient-bg video {
  opacity: 0.10;
  filter: blur(70px) saturate(1.1);
  mix-blend-mode: multiply;
}
/* A vignette so the center stays calm and edges fade to page bg — stops the
   video from competing with content in the reading column. */
.ambient-bg::after {
  content: "";
  position: absolute;
  inset: 0;
  background: radial-gradient(ellipse at 50% 40%,
              transparent 0%,
              var(--bg-main) 75%);
}
/* Accessibility + battery: no motion => show the poster still only. */
@media (prefers-reduced-motion: reduce) {
  .ambient-bg video { display: none; }
  .ambient-bg {
    background: url(/aurora-bg.jpg) center/cover no-repeat;
    opacity: 0.12;
    filter: blur(60px);
  }
}
```

### 0.2 "Text gets brighter when the video is there"

Because the video lives *behind* sections, any section we want to "sit on" the video
gets a class that (a) lets the glow through and (b) brightens its text. Use this on the
hero and on section dividers — NOT on dense reading sections (tour, tables) where we want
calm flat bg.

```css
/* Section that floats over the ambient video: text brightens, weight bumps. */
.over-video { position: relative; }
.over-video h1, .over-video h2 { color: #ffffff; text-shadow: 0 1px 30px rgba(0,0,0,.4); }
.theme-light .over-video h1, .theme-light .over-video h2 { color: var(--text-primary); text-shadow: none; }
.over-video p { color: color-mix(in srgb, var(--text-primary) 88%, white); }
```

Apply `class="hero over-video"` to the hero, and add `over-video` to the two big
"section break" headings (`The fullstack platform`, `Get started`). Everything else keeps
a solid `--bg-main` behind it so the video never touches the reading experience.

### 0.3 Per-page intensity

- **Landing** (`index.lumen`): video visible as described (opacity 0.16).
- **Aurora page**: video *slightly* stronger (opacity 0.20) — it's the flagship, the
  colors are literally Aurora's. Add `theme-aurora` to `<html>`.
- **Lumen page**: NO video, or a warm-tinted version. Lumen is the "bright" page. Either
  drop `.ambient-bg` entirely there, or add `filter: ... hue-rotate(40deg)` to warm it amber.
- **Zenith / Kiln / Constellation / book / stdlib**: NO video. These are "normal" reading
  pages per the brief. Keep them flat and fast. The video is a landing+Aurora signature, not
  a site-wide gimmick — that restraint is what keeps it from looking AI/templatey.

---

## 1. Per-framework hue system (fixes "the icon use is messy")

**The current problem:** every icon on the landing uses `var(--accent)` (one blue). 13
tour icons, 6 feature icons, all identical blue. That monotony is exactly what reads as
"AI generated a page." Color is information here — use it.

### 1.1 Define the framework palette (add to `:root` in `global.css`)

```css
:root {
  --hue-varian:        #4FACFE;  /* core language / general */
  --hue-aurora-1:      #4FACFE;
  --hue-aurora-2:      #00F2FE;
  --hue-zenith:        #5b9cff;  /* backend — calm blue */
  --hue-lumen:         #f5b829;  /* frontend — warm amber/gold */
  --hue-kiln:          #f97316;  /* build — kiln fire orange */
  --hue-constellation: #a78bfa;  /* registry — star violet */
}
```

### 1.2 Ambient glows per framework card (the "ambient ones" the user asked for)

In the **ecosystem section** (the 5 framework blocks: Aurora, Zenith, Lumen, Kiln,
Constellation), give each block a soft radial glow in its own hue behind it. This is the
single highest-impact change for "world-class, not AI." One utility class per framework:

```css
.fw-block { position: relative; isolation: isolate; }
.fw-block::before {
  content: "";
  position: absolute;
  inset: -10% -5%;
  z-index: -1;
  border-radius: var(--radius-2xl);
  opacity: .10;
  filter: blur(80px);
  background: radial-gradient(60% 60% at 50% 0%, var(--fw-hue), transparent 70%);
}
.fw-aurora        { --fw-hue: var(--hue-aurora-2); }
.fw-zenith        { --fw-hue: var(--hue-zenith); }
.fw-lumen         { --fw-hue: var(--hue-lumen); }
.fw-kiln          { --fw-hue: var(--hue-kiln); }
.fw-constellation { --fw-hue: var(--hue-constellation); }
```
Then each framework's icon, its "Learn more" pill border, and any accent text in that
block use `var(--fw-hue)` instead of `var(--accent)`. Result: scrolling the ecosystem
feels like moving through five distinct rooms, each with its own light — instantly more
crafted.

### 1.3 Icon color discipline (the actual cleanup)

Current state to fix:
- **Tour section (13 icons)**: all `color:var(--accent)` inline. Don't rainbow them — that
  would be the opposite mistake. Instead: **drop the per-heading icons entirely** OR make
  them all a single *muted* `--text-muted` and let the ONE active/hovered example glow
  `--accent`. 13 bright blue icons in a column is noise; muted icons with a single accent on
  focus is intentional. Recommended: muted by default.
- **"Why Varian?" + "Everything built in" feature cards**: keep icons, but tint each
  card's icon to the framework it represents where there's a match (the Lumen feature card
  icon → `--hue-lumen`, the Kiln one → `--hue-kiln`, etc.). Generic language features stay
  `--hue-varian`. Now color encodes meaning instead of being uniform.
- **Benchmark table / command chips / inline 14px icons**: these are fine, leave them, but
  set them to `--text-secondary` not `--accent` so they read as UI furniture, not links.
- **Standardize icon weight/size.** Add to `global.css`:
  ```css
  .material-symbols-outlined { font-variation-settings: 'wght' 350, 'opsz' 24, 'GRAD' 0; }
  .feature-icon .material-symbols-outlined { font-variation-settings: 'wght' 400, 'opsz' 28; }
  ```
  Inconsistent icon weights are a subtle "AI" tell; lock them down.

---

## 2. What's missing / what reads as unfinished

Concrete gaps to close (in rough priority order):

1. **Whitespace is too tight for a flagship.** Sections use `--space-12` (3rem) breaks.
   Bump major section vertical padding to `--space-24` (6rem) on desktop, `--space-16` on
   mobile. World-class landing pages breathe. Add a `.section` utility and use it instead of
   the inline `padding: var(--space-12) 0` repeated ~10 times.
2. **Type scale is timid.** Hero `h1` is `--text-5xl` (3rem). For a flagship hero push to
   `clamp(2.75rem, 6vw, 4.5rem)`. Section titles `--text-3xl` → `clamp(1.75rem, 4vw, 2.5rem)`.
   Use `clamp()` everywhere so it's fluid, not stepped — fluid type is a hallmark of
   hand-tuned sites.
3. **No "social proof / at-a-glance stats" band.** Add a thin band under the hero: a row of
   4 stat cards — `147/147 tests`, `~10.5k req/s`, `1 language, whole stack`, `v1.0 shipped`.
   Big number + small label. Cheap, high-credibility, fills the awkward gap between hero and
   first content section.
4. **Hero has no visual anchor besides the code editor.** The ambient video now fixes this —
   but also add a subtle 1px gradient hairline border + soft glow on the editor container so
   it feels like a real product screenshot floating on the gradient.
5. **Footer is bare** (`border-top` + muted text). Build a real 3-column footer: Product
   (Aurora/Zenith/Lumen/Kiln/Constellation), Learn (Book/Stdlib/Tooling/Security), Community
   (GitHub/Issues/PRs). Footers are where AI sites give up; a real one signals care.
6. **No section eyebrows.** Each major section title should have a small uppercase
   letter-spaced "eyebrow" label above it (e.g. `THE ECOSYSTEM`, `PERFORMANCE`, `GET STARTED`)
   in the section's hue. Tiny touch, big "designed" signal.
7. **Cards are flat.** Add the standard treatment: 1px top highlight
   (`box-shadow: inset 0 1px 0 rgba(255,255,255,.06)`) + the lift-on-hover already present.
   On light theme use `inset 0 1px 0 rgba(255,255,255,.8)`.
8. **Nav has no active-section indicator.** `scrollspy.js` exists — wire it so the current
   section's nav link is highlighted in the page hue. (Verify scrollspy.js is actually
   included and functioning; the file is in public/ but confirm the `<script>` tag.)
9. **Mobile nav.** Confirm `.nav-links` collapses to a menu under ~720px; if not, add a
   simple disclosure. A landing that breaks on phones instantly looks unfinished.
10. **Consistency sweep across all sub-pages.** The polish above (spacing, type scale,
    eyebrows, footer, per-page hue, NO video except landing/Aurora) must be applied to every
    page, or the site feels half-done the moment you leave the landing.

---

## 3. Light theme specifically

The video + glows are tuned for dark. On light theme:
- Ambient video opacity → 0.10, `mix-blend-mode: multiply` (already in §0.1).
- Framework glows opacity → 0.06 (they bloom too hard on white): add
  `.theme-light .fw-block::before { opacity: .06; }`.
- Verify `--theme-1`/hue text colors pass contrast on `--bg-main: #FBFCFF`. The amber
  `--hue-lumen #f5b829` fails contrast as body text on white — only use it for icons/borders/
  large headings on light theme, never small text.

---

## 4. Acceptance check (how we know it's world-class, not AI)

- [ ] Ambient video is present on landing + Aurora only, faint, blurred, never fighting text.
- [ ] `prefers-reduced-motion` shows the static poster, no video.
- [ ] Each of the 5 framework blocks has its own hue (glow + icon + accent), no two share.
- [ ] No section has 5+ identical-colored icons in a row.
- [ ] Body text ≥16px everywhere; no line longer than ~70ch.
- [ ] Major sections separated by ≥6rem on desktop.
- [ ] Hero/section titles use fluid `clamp()` type.
- [ ] Real multi-column footer; stat band under hero.
- [ ] Light AND dark both pass a contrast spot-check; video doesn't wash out light mode.
- [ ] Every sub-page got the same spacing/type/footer treatment (no half-done pages).
- [ ] Total added weight: ~760 KB of video (cached, one fetch) — confirm Lighthouse perf
      still green; if not, drop landing video opacity load via `preload="none"` + lazy play.
