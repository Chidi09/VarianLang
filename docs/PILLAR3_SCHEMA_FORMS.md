# Pillar 3 — Schema-bound forms

Goal: a schema-driven form that beats React-Hook-Form + Zod + server-actions by being
server-driven and live: declare fields once (label/type + a `validate` validator), get
auto-rendered inputs, **live per-field validation** as the user types, server-side validation on
submit (single source of truth), and — because a successful submit writes the DB — any Pillar-1
  `live_query` / Pillar-2 table on that table **auto-updates**. The same generated form remains a
  normal named HTML form when enhancement is disabled or unavailable. No client validation lib,
  schema duplication, or manual field wiring.

## Built on what exists
- `vn_modules/validate.vn`: `validate.str()/.num()/.object(fields)` → Validator with `.optional()`,
  `.is_email()/.is_url()/.is_uuid()/.is_alphanumeric()`, `.min(n)/.max(n)`, `.trim()/.strip_html()
  /.escape_html()`, `.parse(v)`. `_validate_parse(validator, v)` runs one.
- `vn_modules/lumen.vn`: `lumen_form(schema)` → `{validate(values) → {ok, values, errors}}`
  (schema = name→validator struct). `_ui_render_field` markup + `.lmn-field*` CSS already exist.
  Component model = `lumen_component(state_fn, render_fn, handler_names, handler_fns)`; handler
  `|s,v|`; field events preserve control semantics and submit events send successful FormData
  controls (including explicit checkbox booleans); morph preserves the focused input's
  value and cursor.

## Design (all new functions in lumen.vn, append-only)
- `_df_make_setter(core, name)` → returns `|s,v| { (core.set_field)(name, v); return s }` (helper so
  per-field handler closures capture the right name — avoids loop-capture bugs).
- `data_form(opts)` headless core. opts: `fields` (array of `{name,label,type,placeholder,hint,
  validator}`), `on_submit`, `initial_values`, `action`, `method`, `enhance`, and `submit_label`. State
  holders: values/errors structs, status (idle|saving|saved|error), message. Closures:
  `values/errors/status/message`, `set_field(name,val)` (updates value + live-validates that one
  field → inline error), `submit()` (validate all via lumen_form → on_submit), `reset()`. Submit
  callbacks may return `{ok,message,errors}` for structured form/field results. Also
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

Runtime-tested. `data_form`, `_data_form_html`, and `data_form_component` support text,
textarea, number, select, checkbox, date, and other native controls; strict unique field
contracts; typed number/boolean coercion; validator transforms; initial-value reset;
structured success and field-error results; accessible labels/help relationships; and
authoritative FormData submission. Every control has a native `name`, and forms retain
`method`/`action`. Set `enhance: false` to emit no Lumen event attributes and therefore zero
optional form JavaScript. With enhancement enabled, only the existing `events` action is
selected. `tests/lumen_data_form_test.vn` covers both modes and the live component lifecycle.

Example: `examples/lumen_data_form.vn` combines a form with a live Pillar-2 table, so a
successful database write refreshes the table. File/multipart streaming is intentionally a
separate upload subsystem rather than hidden inside the scalar form contract.
