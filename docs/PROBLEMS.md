# Known Problems

This file documents known bugs, design issues, and footguns in the Varian
runtime and its associated frameworks.

---

## Heap Corruption (AddressSanitizer-detected)

### `free(): invalid next size (fast)` / `double free or corruption (out)`

A non-deterministic glibc heap corruption crash that occurs during or after
`vn dev pages` in the `website/` directory. The crash is intermittent — it
may not reproduce on every run, and AddressSanitizer does not consistently
detect it (likely because ASan's allocator changes the heap layout).

**Probable root causes** (ordered by likelihood):

1. **`realloc` return value unchecked in `src/lexer.c:400,449`**
   The `string_literal()` function calls `realloc(buf, buf_size)` without
   checking the return value. If `realloc` ever returns NULL (heap
   pressure), `buf` becomes NULL and the subsequent `buf[buf_pos++]`
   writes to address 0+N, corrupting heap metadata.

2. **`int` overflow in `BC_STRING_CONCAT` / `BC_BUILD_STRING`**
   `src/vm.c:4983` computes `int len = a.as.string->length + b.as.string->length`
   using signed `int`. For large strings (>2 GB) this overflows, causing
   `malloc(len + 1)` to allocate a tiny buffer followed by a massive
   `memcpy` past the end. Not triggered at current template sizes (~30 KB
   per page) but a correctness bomb for future large workloads.

3. **Unchecked `realloc` in VM hot paths**
   Multiple allocation sites in `src/vm.c` ignore the `realloc` return
   value: `chunk_grow()`, `gc_push_gray()`, `cache_map_put()`, array
   auto-grow, etc. If `realloc` fails at any of these, the old pointer is
   lost and the next write through the stale pointer corrupts the heap.

4. **String token memory leak in the lexer (`src/lexer.c:412`)**
   Each string literal is `malloc`'d in the lexer and stored in
   `token.value`. The parser copies the value into arena memory via
   `ast_string_literal()` (which calls `arena_alloc` + `strcpy`), but
   the original `malloc`'d buffer is never freed. Over many tokens this
   fragments the heap, making `realloc` failures (item #1) more likely.

**Status:**
- Item #1 (unchecked `realloc`/`malloc` in the lexer) — **Fixed.** Every
  allocation in `string_literal()`/`regex_literal()` now checks for NULL,
  frees the old buffer, and returns `make_error_token(...)` instead of
  writing through a NULL pointer. The regex-flags loop is now bounds-checked.
- Item #2 (signed `int` overflow in string concat) — **Fixed.** `BC_ADD`,
  `BC_STRING_CONCAT` and `BC_BUILD_STRING` now accumulate length in a wider
  type and `RAISE` "result too large" if it would exceed `INT_MAX`, and
  check the `malloc` result.
- Item #3 (unchecked `realloc` in VM hot paths) — **Fixed.** Added
  `vm_xrealloc()` (aborts cleanly on OOM instead of leaking/corrupting) for
  the `void`-context growth sites (`chunk_grow`, chunk RLE/constants,
  `gc_push_gray`, `cache_map_put`); the runtime array-grow site now checks
  and `RAISE`s. `vm_register_task` already guarded.
- Item #4 (lexer string-token leak) — **Fixed** (see "Unnecessary Lexer
  String Copy" below).

---

## Stray Characters in Terminal / Code Blocks ("f" and "k") — RESOLVED

**Actual root cause (not the template engine).** The culprit was the
client-side syntax highlighter `website/public/highlight.js`. It ran one
`String.replace()` per token class, in sequence, inserting markup as it went:
keywords became `<span class="k">…</span>`, functions `<span class="f">…</span>`,
etc. A *later* pass — the string-literal regex `/"([^"]*)"/` — then matched the
quoted class names those earlier passes had just inserted (`"k"`, `"f"`, `"c"`…)
and wrapped them again, breaking the markup so the single-letter class names
leaked into the page as visible text. `k` (keywords) and `f` (functions) are by
far the most frequent spans, which is why those two letters appeared
everywhere. Nothing in the template compiler or `native_lumen_escape_str` was
involved — server-rendered HTML was byte-correct (verified by diffing source
templates against rendered output).

**Fix applied:** rewrote `highlight.js` as a single-pass tokenizer (one ordered
alternation regex, each token classified exactly once via a replacer function),
so no regex can ever re-match previously inserted markup. Verified by
simulation: old highlighter's visible text was `"k">fn greet… "f">print…`; new
highlighter reproduces the source exactly.

**Status:** Fixed.

<details><summary>Original (incorrect) hypothesis, kept for the record</summary>

**Hypothesis:** The Lumen `.lumen` template compiler (specifically
`_lumen_escape_string` / `native_lumen_escape_str` in `src/vm.c:2560`)
escapes `{` → `\{` and `}` → `\}` for embedding template HTML inside a
Varian string literal. At runtime, `\{` is decoded back to `{` by
Varian's string parser. If the template content contains a `{` that is
NOT properly escaped, Varian's string interpolation (`{expr}`) may try
to evaluate it as code, producing partial output that leaks as stray
characters.

**Workaround:** Ensure every `{` and `}` in template HTML is either a
valid Lumen `{{ }}` expression or backslash-escaped `\{` `\}`.
Currently, the `native_lumen_escape_str` function should handle this
(see `src/vm.c:2574`), but a double-encoding or encoding mismatch could
cause issues.

</details>

---

## Non-standard HTML Tags in Code Blocks

The book pages (`book-fundamentals.lumen`, `book-core.lumen`,
`book-advanced.lumen`) originally used non-standard HTML elements for
code-block chrome:

- `<h>` for the header bar (browsers treat as an invalid heading level)
- `<d>` for the coloured dots
- `<t>` for the filename/title text

These confuse the HTML parser and can produce rendering artifacts. They
have been replaced with standard elements:

- `<div class="cb-h">`
- `<span class="cb-d">`
- `<span class="cb-t">`

**Status:** Fixed.

---

## `_lumen_shell()` Double-Document Bug

The `_lumen_shell()` function in `vn_modules/lumen.vn:1670` wraps
rendered HTML in a complete `<!DOCTYPE html><html><head>...</head><body>`
document shell. However, the `.lumen` page templates already produce a
full HTML document (including `<!DOCTYPE>`, `<html>`, `<head>`, `<body>`).
This results in a doubly-nested HTML document when served through the
normal Lumen page route.

**Effect:** The browser's HTML parser engages error recovery, discarding
the page's real `<title>`, stylesheets, and inline scripts. Navigation
via `<a>` links may break.

**Fix applied:** `_lumen_shell()` now detects whether the rendered HTML
contains `<body>` tags. If yes (full document), it injects the
`<div data-lumen-root>` and client JS into the existing document
structure instead of wrapping it in another shell.

**Status:** Fixed.

---

## Missing CSS Scoping Guard

The `lumen_compile_source` function in `vn_modules/lumen.vn:706`
treats any `<style>` block as scoped CSS — it rewrites selectors with
`[data-lumen-css="ComponentName"]` and adds the attribute to every
HTML tag, even when `scoped` is not specified. Only `<style scoped>`
should trigger this behaviour.

**Effect:** Every page with any `<style>` block gets `data-lumen-css`
attributes on every HTML element, bloating the output unnecessarily.

**Fix applied:** `lumen_compile_source` now distinguishes `<style scoped>`
(selector rewriting + `data-lumen-css` attribute injection) from a plain
`<style>` block, which is emitted unchanged as global CSS. Covered by
`tests/lumen_scoped_css_test.vn` (both the scoped and the global case).

**Status:** Fixed.

---

## Unnecessary Lexer String Copy

In `src/lexer.c:326` (`string_literal`), the lexer allocates a
`malloc`'d buffer for every string/identifier token. In `src/parser.c`,
these are copied into arena memory via `ast_string_literal` → `arena_alloc`
→ `strcpy`. The original `malloc`'d buffer is never freed.

**Effect:** Memory leak proportional to the number of string tokens
parsed. For a single compile this is negligible, but in a long-running
`vn dev` session with multiple rebuild cycles it fragments the heap.

**Fix:** Replace the `malloc`/`strcpy` dance with direct arena
allocation in the parser, and remove the `malloc` in the lexer for
tokens that will be immediately consumed by the parser.

**Status:** Not yet fixed.

---

## Built-in Component Composition Crashes (`CRASHED: Cannot dispatch method on this type`)

Using the built-in Lumen UI component vocabulary (`<Section>`, `<Container>`, `<Stack>`, `<Grid>`, `<Card>`, `<Heading>`, `<Text>`, `<Feature>`, `<Button>`) in page templates causes a runtime crash:

```
CRASHED: Cannot dispatch method on this type
```

This error originates from `src/vm.c:4388` in the `BC_DISPATCH` opcode handler. The VM allows method dispatch only on `VAL_STRUCT`, `VAL_MODULE`, `VAL_STRING`, and `VAL_ARRAY`. When something else is used as the receiver of a `.method()` call, this error is raised.

**Root cause isolated (and fixed).** The receiver that could not be
dispatched was `after_head` in `_lumen_shell()` (`vn_modules/lumen.vn`), which
is `nil` rather than a string. It is `nil` because `_lumen_shell` calls the
**one-argument** form `inner.substring(head_close)` (start only, to end of
string), but the native `lib_string_substring` required exactly **three**
args (`arg_count < 3` → `return val_nil()`). The single-arg call therefore
silently returned `nil`, and the next `.substring()` on that `nil` raised
"Cannot dispatch method on this type". This had nothing to do with the
component-composition pipeline or the arena allocator — it fired on every
full-document page that went through `_lumen_shell`.

**Fix applied:** `lib_string_substring` (`src/lib_string.c`) now accepts both
`substring(start)` and `substring(start, end)`; the 1-arg form defaults `end`
to the string length. Regression coverage in `tests/string_substring_test.vn`.
All website routes (`/`, `/lumen`, `/zenith`, `/kiln`, `/constellation`,
`/book-*`, `/the-varian-book`) now return HTTP 200.

**Status:** Fixed.

---

## Stray Characters in Terminal / Code Blocks ("f" and "k")

Users report seeing `f` and `k` as standalone characters inside terminal/code blocks. Likely the Lumen template engine partially evaluating code-example content at render time.

**Hypothesis:** `_lumen_escape_string()` escapes `{` → `\{` for embedding in Varian string literals. If a `{` in template HTML is not properly escaped, Varian's `{expr}` string interpolation may partially evaluate the content, leaking characters.

**Workaround:** Ensure every `{` and `}` in template HTML is either a valid `{{ }}` expression or `\{` `\}`. The native `_lumen_escape_str` (src/vm.c:2574) handles this, but a double-encoding or encoding mismatch could cause issues.

**Status:** Under investigation.
