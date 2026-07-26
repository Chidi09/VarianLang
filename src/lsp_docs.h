#ifndef LSP_DOCS_H
#define LSP_DOCS_H

/* Hover documentation for keywords, primitive types and operators.
 *
 * Sourced from docs/LANGUAGE.md — the authoritative reference — rather than
 * written from memory, so the editor cannot describe semantics the language
 * does not have. Where the reference is explicit about a sharp edge (type
 * annotations being discarded, `??` and `&&` not short-circuiting, `?` being
 * null-propagation rather than Rust's Result operator), the hover says so:
 * those are exactly the points where a user's assumption from another language
 * would be wrong, which is when a hover is worth reading.
 *
 * Looked up by the word under the cursor, so it answers for tokens that never
 * appear as AST nodes — keywords, type names and operators. Before this, every
 * one of those returned a null hover, which is most of what a person actually
 * points at.
 *
 * The `signature` line is rendered as a ```varian fenced block, so it should be
 * real, runnable-looking syntax rather than a prose summary. */

typedef struct {
    const char *name;
    const char *signature;   /* shown as a code block */
    const char *description; /* markdown; keep to a few sentences */
} LspDocEntry;

static const LspDocEntry lsp_keyword_docs[] = {

/* ─── Bindings ─────────────────────────────────────────────────────────── */
{"let", "let x = 10\nlet price: float = 9.99",
 "Declares a binding. At the top level it compiles to a global; inside a "
 "function it becomes a local slot.\n\n"
 "A `: type` annotation is accepted but **parsed and discarded** — the runtime "
 "is fully dynamically typed regardless of what you write. Reassignment does "
 "not repeat `let`."},
{"const", "const MAX = 100",
 "Declares a constant binding. Like `let`, any type annotation is accepted but "
 "not enforced at runtime."},
{"mut", "mut count = 0",
 "Marks a binding as explicitly mutable."},
{"pub", "pub fn handler(req) { ... }",
 "Marks a declaration as public — visible to modules that `use` this file."},

/* ─── Functions ────────────────────────────────────────────────────────── */
{"fn", "fn add(a: int, b: int) -> int {\n    return a + b\n}",
 "Declares a function. Parameter and return annotations are accepted but "
 "**not enforced** — they are for readability and tools like `vn lint`, not "
 "runtime checking.\n\n"
 "Functions may return multiple values, and lambdas/closures are supported."},
{"return", "return a + b\nreturn x, y",
 "Returns from the current function. Multiple comma-separated values return a "
 "tuple."},

/* ─── Control flow ─────────────────────────────────────────────────────── */
{"if", "if x > 10 {\n    print(\"big\")\n} else if x > 0 {\n    print(\"small\")\n}",
 "Conditional execution. Braces are required; the condition needs no "
 "parentheses."},
{"else", "if cond { ... } else { ... }",
 "The alternative branch of an `if`. Chain with `else if`."},
{"while", "while x < 5 {\n    x = x + 1\n}",
 "Loops while the condition holds."},
{"for", "for i in 0..10 {\n    print(i)   // 0 through 9\n}",
 "Iterates over a range or collection. Ranges with `..` are **exclusive** of "
 "the upper bound, so `0..10` yields 0 through 9."},
{"in", "for item in items { ... }",
 "Separates the loop variable from the sequence being iterated in a `for` loop."},
{"loop", "loop {\n    if done { break }\n}",
 "Loops forever until something inside it breaks or returns."},
{"break", "if done { break }",
 "Exits the innermost enclosing loop."},
{"continue", "if skip { continue }",
 "Skips to the next iteration of the innermost enclosing loop."},
{"match", "match value {\n    case 1: print(\"one\")\n    case _: print(\"other\")\n}",
 "Pattern matching over a value, used with `case` arms. Also works with enum "
 "variants."},
{"case", "case 1: print(\"one\")",
 "A single arm of a `match`. `case _` is the catch-all."},

/* ─── Types ────────────────────────────────────────────────────────────── */
{"struct", "struct User {\n    name: string,\n    age: int\n}",
 "Declares a struct type with named fields, and may carry methods via `impl`.\n\n"
 "Note that a plain `struct` declaration parses field type annotations and then "
 "**discards** them; only `schema` retains them for validation."},
{"enum", "enum Status { Active, Banned }",
 "Declares an enum. Variants are matched with `match`/`case` and may carry "
 "payloads."},
{"impl", "impl User {\n    fn greet(self) { ... }\n}",
 "Attaches methods to a type. Methods take `self` as their first parameter."},
{"trait", "trait Printable {\n    fn to_string(self)\n}",
 "Declares a trait. Varian uses **structural** typing: a value satisfies a "
 "trait by having the required methods, with no explicit `impl Trait for` "
 "declaration."},
{"type", "type UserId = int",
 "Declares a type alias."},
{"actor", "actor Counter {\n    count: int\n}",
 "Declares an actor — isolated state with a message mailbox, for concurrency "
 "without shared-memory races."},

/* ─── Modules ──────────────────────────────────────────────────────────── */
{"use", "use \"zenith\"\nuse \"examples/math.vn\"",
 "Imports another module. Resolved as a file path, then as a package directory "
 "under `vn_modules/`, then under `$VARIAN_HOME/vn_modules/`.\n\n"
 "A bare name like `\"zenith\"` needs a *directory* `vn_modules/zenith/`; a "
 "single file must be given with its path and extension."},
{"as", "use \"math\" as m",
 "Binds an import under an alias."},

/* ─── Concurrency ──────────────────────────────────────────────────────── */
{"async", "async fn fetch_user(id) { ... }",
 "Marks a function as asynchronous, so it may suspend at an `await`."},
{"await", "let user = await fetch_user(1)",
 "Suspends until an async call completes. A function containing `await` can "
 "yield to the scheduler mid-body."},

/* ─── Errors ───────────────────────────────────────────────────────────── */
{"try", "try {\n    throw(\"error!\")\n} catch e {\n    print(e)\n}",
 "Runs a block, catching anything thrown inside it."},
{"catch", "catch e { print(e) }",
 "Handles a value thrown inside the matching `try` block. Binding the error "
 "value is optional."},
{"assert", "assert x > 0",
 "Throws if the condition is false."},
{"test", "test \"adds two numbers\" {\n    assert_eq(add(1, 2), 3)\n}",
 "Declares a test, run by `vn test`."},

/* ─── Compile time ─────────────────────────────────────────────────────── */
{"comptime", "let x = comptime { 10 + 20 }   // 30, computed once",
 "Evaluates its body during compilation, in lexical position.\n\n"
 "It can call any function or global defined **earlier** in the program, but "
 "cannot see anything defined after it, nor an enclosing function's locals — it "
 "always runs with no enclosing scope."},

/* ─── Literals ─────────────────────────────────────────────────────────── */
{"true",  "true",  "The boolean true."},
{"false", "false", "The boolean false."},
{"null",  "null",  "The absence of a value.\n\n"
 "Reached safely with `?.`, which short-circuits to `null` rather than erroring, "
 "and defaulted with `??`."},

/* ─── Boolean operator keywords ────────────────────────────────────────── */
{"and", "if active and not banned { ... }",
 "Logical AND. An exact alias for `&&` — both lex to the same token."},
{"or", "if role == \"admin\" or is_owner { ... }",
 "Logical OR. An exact alias for `||` — both lex to the same token."},
{"not", "if not banned { ... }",
 "Logical NOT. An exact alias for `!`."},

/* ─── Primitive type names ─────────────────────────────────────────────── */
{"int", "let n: int = 42",
 "64-bit integer type annotation.\n\n"
 "Annotations are **not enforced** at runtime. Note that comparisons like `<` "
 "and `>=` yield `int` 0/1 rather than `bool`, so `result == false` is false "
 "even when the comparison failed."},
{"float", "let price: float = 9.99",
 "Floating-point type annotation. Not enforced at runtime."},
{"string", "let name: string = \"Chidi\"",
 "String type annotation. Strings are immutable. Not enforced at runtime."},
{"bool", "let ok: bool = true",
 "Boolean type annotation. Only `==` and `!=` produce real booleans; ordering "
 "comparisons yield `int`. Not enforced at runtime."},
{"byte", "let b: byte = 0",
 "Byte type annotation. Accepted by the parser, but there is **no separate "
 "`byte` runtime type** yet — it behaves as an int."},
{"void", "fn log(msg) -> void { ... }",
 "Marks a function as returning nothing meaningful."},
{"any", "fn handle(x: any) { ... }",
 "Opts out of any type annotation for this position."},
};

static const int lsp_keyword_docs_count =
    (int)(sizeof(lsp_keyword_docs) / sizeof(lsp_keyword_docs[0]));

/* Operators, matched on the exact characters under the cursor. Longest match
 * wins, so `?.` is not reported as `?`. */
static const LspDocEntry lsp_operator_docs[] = {
{"?.", "print(missing?.name)   // null, does not error",
 "Nil-safe member access. Short-circuits to `null` when the receiver is null "
 "instead of erroring."},
{"??", "print(missing?.name ?? \"default\")",
 "Returns the left side unless it is null, otherwise the right.\n\n"
 "**It is not short-circuiting** — both sides are always evaluated, consistent "
 "with `&&` and `||` in this VM."},
{"?", "let result = safe_div(a, b)?",
 "Null propagation: if the value is null, the enclosing function returns "
 "immediately.\n\n"
 "This is closer to Swift's optional chaining than to Rust's `Result`-based "
 "`?` — there is no typed error channel."},
{"&&", "if active && !banned { ... }",
 "Logical AND, aliased by the keyword `and`."},
{"||", "if is_admin || is_owner { ... }",
 "Logical OR, aliased by the keyword `or`."},
{"..", "for i in 0..10 { ... }   // 0 through 9",
 "Range operator, **exclusive** of the upper bound."},
{"@", "@cache\nfn expensive(n) { ... }",
 "Applies a decorator. `@cache` memoizes by argument values; `@retry(n)` "
 "restarts the function up to n times if it throws."},
};

static const int lsp_operator_docs_count =
    (int)(sizeof(lsp_operator_docs) / sizeof(lsp_operator_docs[0]));

#endif
