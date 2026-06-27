# Pillar 3 — Schema-bound forms

Goal: a schema-driven form that beats React-Hook-Form + Zod + server-actions by being
server-driven and live: declare fields once (label/type + a `validate` validator), get
auto-rendered inputs, **live per-field validation** as the user types, server-side validation on
submit (single source of truth), and — because a successful submit writes the DB — any Pillar-1
`live_query` / Pillar-2 table on that table **auto-updates**. No client validation lib, no schema
duplication, no manual wiring.

## Built on what exists
- `vn_modules/validate.vn`: `validate.str()/.num()/.object(fields)` → Validator with `.optional()`,
  `.is_email()/.is_url()/.is_uuid()/.is_alphanumeric()`, `.min(n)/.max(n)`, `.trim()/.strip_html()
  /.escape_html()`, `.parse(v)`. `_validate_parse(validator, v)` runs one.
- `vn_modules/lumen.vn`: `lumen_form(schema)` → `{validate(values) → {ok, values, errors}}`
  (schema = name→validator struct). `_ui_render_field` markup + `.lmn-field*` CSS already exist.
  Component model = `lumen_component(state_fn, render_fn, handler_names, handler_fns)`; handler
  `|s,v|`; client sends `el.value` as `v` on `@input`/`@submit`; morph preserves the focused
  input's value+cursor.

## Design (all new functions in lumen.vn, append-only)
- `_df_make_setter(core, name)` → returns `|s,v| { (core.set_field)(name, v); return s }` (helper so
  per-field handler closures capture the right name — avoids loop-capture bugs).
- `data_form(opts)` headless core. opts: `fields` (array of `{name,label,type,placeholder,hint,
  validator}`), `on_submit` (fn(values)->any; throwing => error status), `submit_label`. State
  holders: values/errors structs, status (idle|saving|saved|error), message. Closures:
  `values/errors/status/message`, `set_field(name,val)` (updates value + live-validates that one
  field → inline error), `submit()` (validate all via lumen_form → on_submit), `reset()`. Also
  returns `fields`, `names`, `submit_label`.
- `_data_form_html(core)`: `<form @submit="df_submit">` with each field rendered as
  `.lmn-field` (label + input/textarea with `@input="df_set_<name>"` + inline `.lmn-field-error`),
  a primary submit button, and a status message. Run through `lumen_render` so events bind.
- `data_form_component(opts)`: factory → `lumen_component` with handler_names
  `["df_submit","df_reset", "df_set_<each field>"]` and matching handlers (per-field via
  `_df_make_setter`). Mount with `lumen_mount`.
- CSS: `.lmn-form`, `.lmn-input`, `.lmn-form-actions`, `.lmn-form-msg` (success/danger) added to
  `_lumen_design_css()`. (`.lmn-field*` already exist.)

## Status
Chunk 1 DONE — `_df_make_setter`, `data_form`, `_data_form_html`, `data_form_component` + form CSS
added to `lumen.vn` (mirrored to deploy). Example: `examples/lumen_data_form.vn` (form + a live
Pillar-2 table on the same DB table → the table auto-refreshes on submit). Untested at runtime
(Windows LSP-only / `.vn` interpreted) — needs Linux/WSL. Later: input types
(select/checkbox/radio/date), `@input` debounce modifier, optimistic submit, file upload.
