; Symbol outline for Zed (breadcrumbs + outline panel).
; Only references nodes/fields that exist in the compiled grammar.

(function_definition
  "fn" @context
  name: (identifier) @name) @item

(struct_definition
  "struct" @context
  name: (type_identifier) @name) @item

(enum_definition
  "enum" @context
  name: (type_identifier) @name) @item

(actor_definition
  "actor" @context
  name: (identifier) @name) @item

(trait_definition
  "trait" @context
  name: (type_identifier) @name) @item

(impl_block
  "impl" @context
  type: (type_identifier) @name) @item

(type_alias
  "type" @context
  (identifier) @name) @item

(test_definition
  "test" @context
  (string) @name) @item
