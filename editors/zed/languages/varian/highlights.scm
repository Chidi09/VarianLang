; Tree-sitter highlight queries for Varian (consumed by Zed and Neovim).
; Requires the tree-sitter-varian grammar (see editors/tree-sitter-varian/).

[
  "let" "const" "fn" "struct" "enum" "actor" "impl" "trait" "type"
  "use" "pub" "mut" "test" "comptime"
] @keyword

[
  "return" "if" "else" "while" "for" "in" "loop" "match" "case"
  "break" "continue" "try" "catch" "async" "await"
] @keyword.control

["and" "or" "not" "as" "assert"] @keyword.operator

[(true) (false) (null)] @constant.builtin

["bool" "int" "float" "string" "byte" "void"] @type.builtin

(comment) @comment
(string) @string
(escape_sequence) @string.escape
(number) @number

(function_definition name: (identifier) @function)
(call_expression function: (identifier) @function)
(type_identifier) @type

["+" "-" "*" "/" "%" "==" "!=" "<" ">" "<=" ">=" "=" "->" "<-" ".." "|"] @operator
["(" ")" "{" "}" "[" "]"] @punctuation.bracket
["," "." ":" ";"] @punctuation.delimiter
