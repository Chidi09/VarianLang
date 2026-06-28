# Linter and Formatter Gap Analysis Report

## Overview

The linter (`src/lint.c`) and formatter (`src/fmt.c`) have fallen behind the rapid evolution of Varian's language features and standard library modules. This report documents the specific gaps and provides a step-by-step implementation plan to bring them up to date.

---

## Part 1: Linter Gaps (`src/lint.c`)

### 1.1 Unhandled AST Node Kinds

The `lint_walk` function's `switch` statement (line 351) handles 30 of 48 node kinds defined in `include/ast.h`. **18 node kinds are unhandled** (fall through to `default: break`):

| Node Kind | Severity | Impact |
|-----------|----------|--------|
| `NODE_CONST_DECL` | HIGH | `const` declarations invisible — no shadow check, unused check, or secret detection |
| `NODE_QUESTION_DOT` | HIGH | `expr?.member` expressions never walked — misses unused bindings, secrets |
| `NODE_ENUM_LITERAL` | HIGH | `Enum::Variant(val)` payload values never walked |
| `NODE_TRAIT_DECL` | HIGH | Trait declarations not validated for duplicate methods |
| `NODE_MATCH_ARM` | MEDIUM | Match arm patterns and bind_names not walked |
| `NODE_AWAIT` | MEDIUM | `await expr` — expression never walked |
| `NODE_ASSERT` | MEDIUM | `assert cond` — condition never walked |
| `NODE_PROPAGATE` | MEDIUM | `expr?` — expression never walked |
| `NODE_COMPTIME` | MEDIUM | `comptime { body }` — body never walked |
| `NODE_FFI_DECL` | MEDIUM | FFI declarations not checked for consistency |
| `NODE_BREAK` | LOW | No validation that break is inside a loop |
| `NODE_CONTINUE` | LOW | No validation that continue is inside a loop |
| `NODE_INT_LITERAL` | LOW | No children to walk |
| `NODE_FLOAT_LITERAL` | LOW | No children to walk |
| `NODE_BOOL_LITERAL` | LOW | No children to walk |
| `NODE_NULL_LITERAL` | LOW | No children to walk |

### 1.2 Missing Native Methods in `known_native_methods`

The array at lint.c:156-172 lists 55 native methods. At least 45+ registered dispatch methods are missing, making `impl Type { fn min(self) }` collision detection incomplete:

**Critical missing entries (used heavily in vn_modules):**
- `"get_field"`, `"get_keys"` — from `lib_validate.c` (used in every vn_module via `_validate.get_field()`)
- `"trim"`, `"starts_with"`, `"ends_with"`, `"code_at"` — string methods
- `"replace"`, `"split"`, `"contains"`, `"is_alphanumeric"`, `"is_email"`, `"is_uuid"`, `"is_url"`
- `"drain_changes"` — sqlite reactive hook

**Math methods missing:**
- `"min"`, `"max"`, `"clamp"`, `"lerp"`, `"round"`, `"trunc"`, `"sign"`
- `"pow"`, `"exp"`, `"log"`, `"log2"`, `"log10"`, `"sqrt"`
- `"sin"`, `"cos"`, `"tan"`, `"asin"`, `"acos"`, `"atan"`, `"atan2"`
- `"sinh"`, `"cosh"`, `"tanh"`
- `"deg_to_rad"`, `"rad_to_deg"`
- `"random"`, `"randint"`, `"randfloat"`, `"seed"`, `"shuffle"`
- `"sum"`, `"product"`, `"mean"`, `"median"`, `"stddev"`, `"variance"`, `"min_arr"`, `"max_arr"`

**Auth/crypto methods missing:**
- `"hash_password_v2"`, `"verify_password_v2"`
- `"sign_jwt_v2"`, `"verify_jwt_v2"`
- `"encrypt"`, `"decrypt"`
- `"totp_generate"`, `"totp_verify"`, `"totp_secret"`
- `"generate_token"`, `"hash_sha256"`, `"constant_time_eq"`

**HTTP methods missing:**
- `"post_stream"`, `"parse_multipart"`, `"test_request"`

**Other missing:**
- `"explain"`, `"make"` — from `lib_errors.c`
- `"build"`, `"select"`, `"fields"`, `"where"`, `"order_by"`, `"paginate"`, `"limit"`, `"offset"` — db query builder
- `"send"` — from smtp

### 1.3 Stubbed/Incomplete Module Checks

1. **Fetch check** (lines 227-237): Empty check body with only comments. The actual fetch detections are in the `NODE_EXPR_STMT` case (lines 430-452), but the `check_module_usages` version is dead code.

2. **Rate-limit/cors cluster check** (lines 298-310): TODO comments and a naive implementation at lines 613-616 that flags **every** `listen_cluster` call as insecure, without checking whether cors/rate_limit middleware is actually registered. Produces false positives.

3. **No checks for new modules:**
   - `ws.vn` — WebSocket usage patterns (missing close handlers, buffer management)
   - `shield.vn` — CSRF middleware ordering, CORS misconfiguration
   - `queue.vn` — job handler registration before submission
   - `observe.vn` — logger naming conventions, span lifecycle
   - `ai.vn` — hardcoded API keys in source
   - `storage.vn` — path traversal in key names
   - `cache.vn` — TTL management (missing expire)
   - `ratelimit.vn` — missing Redis connection checks
   - Lumen — handler arity mismatches (handlers receive (state, value))
   - Lumen — `import ... from "..."` validation

---

## Part 2: Formatter Gaps (`src/fmt.c`)

### 2.1 Missing Keywords in `fmt_keywords` Table

The keyword table at fmt.c:272-286 is missing:

| Missing Keyword | Severity | Impact |
|----------------|----------|--------|
| `"schema"` | HIGH | Schema declarations formatted as identifier instead of keyword — `schema` gets no trailing space, opening brace gets wrong spacing |
| `"ffi"` | LOW | FFI declarations not recognized (but `ffi` appears inside `@ffi(...)` which is a decorator call, not a standalone keyword — so impact is minimal) |

### 2.2 Decorator `@` Token Formatting Bug

**Severity: HIGH.** The `@` character is classified as `FMT_OPERATOR` (line 254) and falls through to the general operator rule at lines 798-804:

```c
if (!prev_was_nl && !prev_was_punct) EMITS(" ");
EMIT(tok->start, tok->length);
EMITS(" ");  // <-- always emits trailing space
```

This means:
```
@validate
```
formats as:
```
 @validate
```

The trailing space after `validate` is wrong — `@validate` should be followed by whatever comes next (a struct, fn, field) without an extra space between the decorator name and the declaration.

### 2.3 Token Classifications

The formatter lumps several important constructs into `FMT_OPERATOR` with no dedicated token type:

| Symbol | Current Type | Problem |
|--------|-------------|---------|
| `@` | `FMT_OPERATOR` | Decorators get wrong spacing (always trailing space) |
| `#` | `FMT_OPERATOR` | Attribute-like constructs get wrong spacing |
| `\` | `FMT_OPERATOR` | Line continuation gets wrong spacing |

Adding `FMT_DECORATOR` (or `FMT_AT`) would let decorators format correctly: `@name` without trailing space on `name`.

### 2.4 Other Formatter Issues

1. **`async` not is_decl:** `async` is in the keyword table but `is_decl = false`. `async fn foo()` may not format the space between `async` and `fn` correctly.

2. **Channel send/receive operator:** `<-` always gets spaces around it (`channel <- value`), but `<-channel` (prefix receive) should ideally have no leading space when used as prefix. Current behavior is acceptable but suboptimal.

3. **Object literals `{ key: val }` vs blocks:** The formatter cannot distinguish object literals from code blocks since it's token-based, not AST-based. Both get the same brace-block treatment. The `:` always gets `: ` formatting, which works for object literals incidentally.

4. **Struct literal field formatting:** No special handling for `Struct { field1: val1, field2: val2 }` — relies on general comma/brace formatting which works but doesn't align fields vertically.

---

## Part 3: Implementation Plan

### Phase 1: Linter — Walk All Node Kinds (1-2 days)

**Step 1.1:** Add cases for each missing node kind in `lint_walk`:

1. `NODE_CONST_DECL` — copy `NODE_LET_DECL` logic (initializer walk, binding registration, shadow check, secret detection)
2. `NODE_QUESTION_DOT` — walk `object` and check `member` as usage (like `NODE_MEMBER` does)
3. `NODE_ENUM_LITERAL` — walk all `values` in the enum literal
4. `NODE_MATCH_ARM` — walk `body` and `pattern`; add bind_names to scope
5. `NODE_TRAIT_DECL` — check for duplicate method names (like struct duplicate field check)
6. `NODE_AWAIT` — walk the awaited `expr`
7. `NODE_ASSERT` — walk the `condition`
8. `NODE_PROPAGATE` — walk the `expr`
9. `NODE_COMPTIME` — walk the `body`
10. `NODE_FFI_DECL` — minimal: walk param names, check lib/func name presence
11. `NODE_BREAK` — add `in_loop` guard with error if outside loop
12. `NODE_CONTINUE` — add `in_loop` guard with error if outside loop

**Step 1.2:** Audit and populate `known_native_methods`:

1. Grep every `vm_register_dispatch` call across all `lib_*.c` files
2. Add every registered method name to the array (sorted alphabetically)
3. Double-check that known_native_methods is automatically kept in sync (add a compile-time assertion or a comment pointing to the audit)

### Phase 2: Linter — Fix Stubbed Checks (1 day)

**Step 2.1:** Rewrite `check_module_usages` fetch check:

1. Remove the empty block at lines 227-237 (the real check is in NODE_EXPR_STMT and works correctly)
2. Or, refactor to consolidate fetch checking logic in one place

**Step 2.2:** Fix `listen_cluster` rate-limit check:

1. Change from flagging every `listen_cluster` to tracking whether `shield.cors()` or `ratelimit.check()` is registered in the same file/session
2. Or, downgrade to an informational note that doesn't produce a hard error

### Phase 3: Linter — New Module-Specific Checks (2-3 days)

**Step 3.1:** Add Lumen-specific checks:

1. Validate handler arity — Lumen handlers are `|state, value|` functions; flag if a handler has != 2 params
2. Validate `@click` style event names in templates — warn if event name doesn't match a registered handler
3. Validate `import ... from "..."` paths resolve to existing files

**Step 3.2:** Add security checks:

1. Detect `ai.vn` API key hardcoding (`openai_api_key`, `anthropic_api_key` as string literals)
2. Detect `storage.vn` path traversal vulnerabilities in key construction
3. Detect missing `ttl` or `expire` calls on `cache.vn` entries

**Step 3.3:** Add correctness checks:

1. `ws.vn` — flag WebSocket `.close()` not called in handler path
2. `queue.vn` — flag job handler defined but not registered
3. `shield.vn` — flag CORS with `*` origin in production
4. `observe.vn` — flag duplicate span names

### Phase 4: Formatter — Keyword and Token Fixes (1 day)

**Step 4.1:** Add `"schema"` to `fmt_keywords` table at line 272 with `is_decl = true`.

**Step 4.2:** Fix `@` decorator formatting:

1. Option A: Change `@` from `FMT_OPERATOR` to a new `FMT_DECORATOR` token type. Handle it before the operator section with its own format rule: emit `@` without leading space (when preceded by newline/indent), skip the operator trailing space.
2. Option B: Add a special case within the `FMT_OPERATOR` handler (lines 613-805) for single-char `@` — check if the next token is an identifier and suppress the trailing space.

**Step 4.3:** Audit `fmt_is_opening_brace_keyword` to include `"schema"`.

### Phase 5: Formatter — Construct-Specific Improvements (2 days)

**Step 5.1:** Improve `async` handling:

1. Add `"async"` to the opening-brace keyword list or handle `async fn` sequences specially
2. Ensure `async fn foo()` preserves the space between keywords when `async` is not `is_decl`

**Step 5.2:** Improve channel operator formatting:

1. When `<-` is used as a prefix operator (after newline, assign, paren open), suppress the leading space
2. When `<-` is used as a binary operator (between two expressions), keep spaces around it

**Step 5.3:** (Optional) Add object literal brace detection:

1. When a `{` at expression level is followed by `key:` pattern, treat as inline object literal instead of a block that needs newline-separated body

### Phase 6: Formatter — Lumen SFC Improvements (0.5 day)

**Step 6.1:** Add `<client>` block recognition to `fmt_format_lumen_source`:

1. Currently handles `<template>`, `<style>`, `<script>` blocks
2. Add handling for `<client>...</client>` block — format as JavaScript (or raw text)

### Phase 7: Testing and Verification (1 day)

**Step 7.1:** Create test files that exercise every newly handled construct:

- One `.vn` file per new node kind for lint tests
- One `.vn` file with all constructs for format round-trip tests
- `.lumen` SFC file with all block types for format round-trip tests

**Step 7.2:** Run existing test suite to verify no regressions:

```bash
make test
make lint
```

**Step 7.3:** Run the formatter on all files in the repository to produce a diff. Verify the diff is clean (no change) for files that were already formatted, and that files with new constructs format correctly.

---

## Summary

The linter needs **18 new case handlers** in `lint_walk`, **~45+ additions** to `known_native_methods`, and **3 rewritten module checks**. The formatter needs **1 keyword addition** (`schema`), **1 token type improvement** (`@` decorator), and **3 construct-specific format refinements**. Total estimated effort: **8-10 days** for one developer familiar with the codebase.
