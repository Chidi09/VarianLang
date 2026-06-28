# Implementation Plan for Linter and Formatter

You will implement the fixes directly into `src/lint.c` and `src/fmt.c`. Execute these precise steps.

## Phase 1: Update Formatter (`src/fmt.c`)
1. In `fmt_keywords` (around line 272), add `{"schema", true},` and `{"ffi", true},` to the list before `{NULL, false}`.
2. In the token formatting loop for `FMT_OPERATOR`, find the block labeled `/* Other operators — space around */` (around line 798). Replace it with:
```c
            /* Other operators — space around */
            if (tok->length == 1 && tok->start[0] == '@') {
                if (!prev_was_nl && !prev_was_punct) EMITS(" ");
                EMIT(tok->start, tok->length);
            } else {
                if (!prev_was_nl && !prev_was_punct) EMITS(" ");
                EMIT(tok->start, tok->length);
                EMITS(" ");
            }
            prev = *tok;
            prev_is_decl_keyword = false;
            continue;
```
3. Update `fmt_is_opening_brace_keyword` to return `true` for `"schema"`.

## Phase 2: Update Linter (`src/lint.c`)
1. In `known_native_methods`, add the missing methods in ALPHABETICAL ORDER:
   `"acos"`, `"asin"`, `"atan"`, `"atan2"`, `"build"`, `"clamp"`, `"cosh"`, `"decrypt"`, `"deg_to_rad"`, `"drain_changes"`, `"encrypt"`, `"exp"`, `"fields"`, `"hash_password_v2"`, `"lerp"`, `"limit"`, `"log"`, `"log10"`, `"log2"`, `"max"`, `"max_arr"`, `"mean"`, `"median"`, `"min"`, `"min_arr"`, `"offset"`, `"order_by"`, `"paginate"`, `"parse_multipart"`, `"post_stream"`, `"pow"`, `"product"`, `"rad_to_deg"`, `"randfloat"`, `"randint"`, `"random"`, `"round"`, `"seed"`, `"select"`, `"shuffle"`, `"sign"`, `"sign_jwt_v2"`, `"sinh"`, `"stddev"`, `"sum"`, `"tanh"`, `"totp_generate"`, `"totp_secret"`, `"totp_verify"`, `"trunc"`, `"variance"`, `"verify_jwt_v2"`, `"verify_password_v2"`, `"where"`.

2. In `lint_walk`, add `case` handlers that properly walk the children for the missing node kinds. Read `include/ast.h` to ensure you use the exact correct struct members (e.g. `node->unary.expr`, `node->assert_stmt.cond`, etc.). 
   - `NODE_CONST_DECL`: walk initializer, register the local.
   - `NODE_QUESTION_DOT`: walk object, register member usage.
   - `NODE_MATCH_ARM`: walk pattern, walk body.
   - `NODE_ENUM_LITERAL`: walk values (if any).
   - `NODE_AWAIT`, `NODE_PROPAGATE`: walk inner expr.
   - `NODE_ASSERT`: walk condition.
   - `NODE_COMPTIME`: walk body.
   - `NODE_BREAK`, `NODE_CONTINUE`, `NODE_INT_LITERAL`, `NODE_FLOAT_LITERAL`, `NODE_BOOL_LITERAL`, `NODE_NULL_LITERAL`, `NODE_TRAIT_DECL`, `NODE_FFI_DECL`: add empty cases with `break;` so they don't fall through to default.

3. Fix the stubbed `check_module_usages`. Remove the dead fetch code block at lines 227-237 (the real check is in `NODE_EXPR_STMT`). For the `listen_cluster` rate-limit check, disable the hardcoded naive error (remove or comment it out) so it stops producing false positives, or change it to just an informational note.

Execute these updates on `src/fmt.c` and `src/lint.c`. Do not leave things half-done.
