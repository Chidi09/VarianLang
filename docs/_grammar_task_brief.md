# Task: tree-sitter-varian grammar — contextual schema + Lumen, regenerate, push

Work ONLY in a fresh clone of the grammar repo (NOT the VarianLang main repo). You own it.
Start every shell with:
```bash
export PATH="$HOME/scoop/shims:$HOME/scoop/apps/nodejs/current:$HOME/AppData/Roaming/npm:$PATH"
```

## Setup
```bash
node --version    # should print (nodejs is installed via scoop)
npm install -g tree-sitter-cli@0.22.6   # MUST be 0.22.6 — Zed ABI compatibility (matches current parser)
tree-sitter --version
cd /c/Users/X1/AppData/Local/Temp && rm -rf tsv && git clone https://github.com/Chidi09/tree-sitter-varian tsv && cd tsv
```

## Grammar changes (`grammar.js`)
1. **Contextual `schema`** — add a `schema_definition` rule for declarations: `schema <identifier> { <fields> }`.
   The literal `schema` must be consumed ONLY in this declaration production. A bare `schema`
   elsewhere MUST still parse as a normal identifier. Do NOT add `schema` to any global
   keyword list / identifier-exclusion. (Reason: `let schema = http.create_struct(...)` is used
   all over the Varian stdlib — reserving `schema` breaks it.)
2. **`dispatch_call`** node for method calls `obj.method(args)`, and **`self`** — so the editor
   highlight queries (which reference `(dispatch_call ...)`, `"self"`, `"schema"`) resolve to
   real nodes.
3. **Lumen HTML support** — so `.lumen` `<template>…</template>` parses as HTML with embedded
   Varian in `<script>` and `{{ }}`. Keep `.vn` parsing behavior unchanged.

## Validate (MUST pass — this is the acceptance gate)
```bash
tree-sitter generate
# 'schema' as an IDENTIFIER must NOT produce ERROR nodes:
tree-sitter parse "/c/Users/X1/CHIDIS WORKSPACE/VarianLang/vn_modules/lumen.vn" 2>&1 | grep -i ERROR | head
tree-sitter parse "/c/Users/X1/CHIDIS WORKSPACE/VarianLang/tests/lumen_form_test.vn" 2>&1 | grep -i ERROR | head
# a schema DECLARATION should parse as schema_definition — make a tiny sample and check:
printf 'schema User { name: string }\nlet schema = 5\n' > /tmp/s.vn
tree-sitter parse /tmp/s.vn
```
Both real files must show NO ERROR around `schema` identifiers; the sample must show a
`schema_definition` for the declaration AND treat `let schema = 5` as a normal identifier.

## Commit + push
```bash
git add -A
git commit -m "grammar: contextual schema_definition, dispatch_call, self, Lumen HTML injection (regen w/ tree-sitter-cli 0.22.6)"
git push origin main
echo "NEWSHA=$(git rev-parse HEAD)"
```

## Report
- `NEWSHA` (the pushed commit).
- The `tree-sitter parse` ERROR-grep results for both real files (should be empty).
- Confirmation the sample shows `schema_definition` for the declaration but identifier for `let schema`.
- Anything you couldn't finish.

Do NOT touch the VarianLang main repo. Do NOT run `make`.
