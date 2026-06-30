; Varian syntax highlighting (Zed / Neovim / tree-sitter editors).
;
; IMPORTANT: every (node), field:, and "token" referenced here MUST exist in the
; compiled grammar (editors/tree-sitter-varian/src/node-types.json). A single
; unknown reference makes tree-sitter reject the ENTIRE query, which produces
; zero highlighting. This file was validated against the current grammar.

; ── Keywords ──────────────────────────────────────────────
[
  "let" "const" "mut" "fn" "struct" "enum" "actor" "impl" "trait" "type"
  "pub" "test" "comptime" "async" "schema"
] @keyword

[
  "return" "if" "else" "while" "for" "in" "loop" "match" "case"
  "break" "continue" "try" "catch" "throw" "assert" "await"
] @keyword.control

["and" "or" "not"] @keyword.operator

; ── Primitive types ───────────────────────────────────────
[
  "bool" "int" "float" "string" "byte" "void"
  "ptr" "c_int" "c_double" "c_float" "c_char"
] @type.builtin

(type_identifier) @type

; ── Functions ─────────────────────────────────────────────
(function_definition name: (identifier) @function)
(anonymous_function) @function
(lambda_expression) @function

(call_expression function: (identifier) @function.call)


(dispatch_call method: (identifier) @function.method)

; ── Member / property access ──────────────────────────────
(member_expression property: (identifier) @property)

; ── Declarations / parameters ─────────────────────────────
(let_declaration name: (identifier) @variable)
(const_declaration name: (identifier) @constant)
(mut_declaration name: (identifier) @variable)
(parameter name: (identifier) @variable.parameter)

; ── Structs ───────────────────────────────────────────────
(struct_definition name: (type_identifier) @type)
(struct_field name: (identifier) @property)
(struct_literal type: (type_identifier) @type)

; ── Enums ─────────────────────────────────────────────────
(enum_definition name: (type_identifier) @type)
(enum_variant name: (identifier) @constant)
(enum_literal
  type: (type_identifier) @type
  variant: (identifier) @constant)

; ── Actors / traits / impls ───────────────────────────────
(actor_definition name: (identifier) @type)
(trait_definition name: (type_identifier) @type)
(impl_block type: (type_identifier) @type)

; ── Literals ──────────────────────────────────────────────
(number) @number
(boolean) @constant.builtin
(null) @constant.builtin
(string) @string
(single_quote_string) @string
(interpolated_string) @string
(byte_slice) @string.special
(regex_literal) @string.regexp

; ── Comments ──────────────────────────────────────────────
(comment) @comment
(block_comment) @comment

; ── Decorators / FFI ──────────────────────────────────────
(decorator) @attribute
(field_decorator) @attribute
(ffi_declaration) @keyword

; ── Operators ─────────────────────────────────────────────
[
  "+" "-" "*" "/" "%"
  "==" "!=" "<" ">" "<=" ">="
  "=" "+=" "-=" "*=" "/="
  "->" "<-" "=>" ".." "::"
  "&&" "||" "!" "?" "??" "?."
  "&" "|" "^" "~"
] @operator

; ── Punctuation ───────────────────────────────────────────
["(" ")" "{" "}" "[" "]"] @punctuation.bracket
["," "." ":" ";"] @punctuation.delimiter

; ── Framework APIs (Zenith, Lumen, Aurora) ────────────────
; Zenith backend framework methods
(dispatch_call
  method: (identifier) @function.builtin
  (#match? @function.builtin "^(get|post|put|delete|patch|options|head|add_middleware|serve_static|enable_docs|on_error|handle|listen|listen_cluster|listen_tls|listen_tls_cluster|register_schema)$"))

; Lumen frontend framework functions
(call_expression
  function: (identifier) @function.builtin
  (#match? @function.builtin "^(lumen_mount|lumen_mount_dynamic|lumen_mount_data|lumen_render|lumen_render_response|lumen_component|lumen_register_component|lumen_compile_source|lumen_compile_file)$"))

; Standard library / framework module namespaces
((identifier) @namespace
  (#match? @namespace "^(http|json|crypto|auth|validate|sanitize|regex|io|math|env|time|sqlite|postgres|redis|zenith|lumen|fetch|cache|ws|queue|mail|task|errors|_validate|_sanitize|_tpl)$"))

; ── Lumen Template-Specific Captures ──────────────────────
; HTML tags in templates (captured as identifiers in Varian grammar)
((identifier) @tag
  (#match? @tag "^(template|div|span|h1|h2|h3|h4|h5|h6|p|a|button|input|form|label|select|option|textarea|ul|ol|li|table|tr|td|th|thead|tbody|img|br|hr|section|article|nav|header|footer|main|aside|figure|figcaption|mark|code|pre|blockquote|cite|em|strong|small|sub|sup|u|s|script|style)$"))

; Standard HTML attributes
((identifier) @property
  (#match? @property "^(class|id|name|href|src|alt|type|value|placeholder|action|method|for|rows|cols|title|disabled|checked|selected|readonly|required|multiple|target|rel)$"))

; Template directives (@click, @submit, :bind, :class, etc.)
(decorator
  (identifier) @keyword.directive
  (#match? @keyword.directive "^(click|submit|change|input|focus|blur|keydown|keyup|mouseenter|mouseleave|scroll|resize|load|error|show|hide|toggle|bind|class|style|model|if|for|each|ref|key|slot)$"))

; Template interpolation brackets {{ }}
("{" @punctuation.special
  (#match? @punctuation.special "^\\{$"))
("}" @punctuation.special
  (#match? @punctuation.special "^\\}$"))
