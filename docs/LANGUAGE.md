# Varian Language Reference

This document describes the language **as it actually works today**, verified against
the lexer/parser/VM and the example files in `examples/` and `tests/`. It intentionally
does not cover anything from `VARIAN_Complete_Feature_Spec.docx` that isn't implemented
yet — see `PLAN.md` for the roadmap of what's still aspirational.

## Variables

```varian
let x = 10
let name = "Chidi"
let active = true
```

`let` declares a binding. At the top level, `let` compiles to a global; inside a
function, it's a local slot. Optional type annotations are accepted after `:` but are
**parsed and discarded** — the runtime is fully dynamically typed regardless of any
annotation written:

```varian
let price: float = 9.99   // ": float" has no runtime effect today
```

Reassigning an existing binding doesn't need `let` again:

```varian
let attempt = 0
attempt = attempt + 1
```

## Primitive types

`bool`, `int`, `float`, `string`, arrays (`[1, 2, 3]`), tuples, structs, enums, and
`null`. Strings are immutable. There is no separate `byte`/`rune` runtime distinction
yet even though those type-annotation keywords exist.

## Functions

```varian
fn greet(name, greeting) {
    return greeting + ", " + name + "!"
}

fn add(a: int, b: int) -> int {
    return a + b
}
```

Parameter and return type annotations are accepted but, like `let`, are not enforced —
useful for readability and for tools like `vn lint`, not for runtime checking.

### Multiple return values

```varian
fn divide(a, b) {
    return a, b   // or: return a / b, nil  — comma-separated values
}
```

### Lambdas and closures

```varian
let double = |x| { return x * 2 }
print(double(21))
```

Lambdas **capture enclosing locals and parameters by value** (copied at the moment the
closure is created, not live shared cells):

```varian
struct Box { val: int }
impl Box {
    fn make_adder(self) {
        return |x| { return self.val + x }
    }
}
let b = Box { val: 10 }
let adder = b.make_adder()
print(adder(5))   // 15 — `self` was captured into the closure
```

If a captured value is itself a struct or array (heap-allocated, reference semantics —
see below), mutations made through that reference after capture are still visible,
since the closure copies the *pointer*, not a deep copy of the data.

## Mutation semantics — read this before you get surprised

**Structs mutate in place** when you call a method on them — there is no need to
reassign:

```varian
struct Box { val: int }
impl Box {
    fn bump(self) { self.val = self.val + 1 }
}
let b = Box { val: 1 }
b.bump()
print(b.val)   // 2 — bump() mutated the same object `b` refers to
```

**Arrays are copy-on-write** via `.push()` — it always returns a *new* array and never
mutates the original:

```varian
let a = [1, 2, 3]
let b = a.push(4)
print(a.len())   // 3 — unchanged
print(b.len())   // 4
```

This asymmetry is real and currently undocumented anywhere else — the idiom for
"appending" to a struct field that holds an array is always `self.field =
self.field.push(x)`, reassigning the field (which *does* persist, since that's a struct
mutation), not mutating the array value itself.

Index assignment (`arr[i] = x`) works correctly and mutates in place.

## Control flow

```varian
if x > 10 {
    print("big")
} else if x > 0 {
    print("small")
} else {
    print("non-positive")
}

while x < 5 {
    x = x + 1
}

for i in 0..10 {
    print(i)   // exclusive range: 0..9
}

loop {
    if done { break }
}
```

## Nil safety

```varian
let u = User { name: "Alice" }
let missing = null

print(u?.name)          // "Alice"
print(missing?.name)    // nil — short-circuits instead of erroring
print(missing?.name ?? "default")   // "default"
```

`?.` short-circuits to `nil` if the object is nil, instead of erroring. `??` evaluates
both sides (it is **not** short-circuiting — consistent with `&&`/`||` in this VM, which
also always evaluate both operands) and returns the left side unless it's `nil`.

## Structs and methods

```varian
struct User {
    id: string,
    name: string,
    email: string
}

impl User {
    fn display_name(self) -> string {
        return self.name + " <" + self.email + ">"
    }
}

let u = User { id: "1", name: "Chidi", email: "chidi@example.com" }
print(u.display_name())
```

Struct fields can be declared without a type at all (`handler,` with no `: Type`) —
useful for fields meant to hold functions/closures or genuinely dynamic values.

### Field validation decorators

```varian
struct SignupForm {
    @is_email email: string,
    @min_len(3) name: string,
}

let ok = SignupForm { email: "a@b.com", name: "Alice" }       // fine
let bad = SignupForm { email: "not-an-email", name: "Al" }    // throws at construction
```

Validation runs automatically every time a struct literal of that type is constructed,
matched by field name (not by position — field order in the literal doesn't need to
match declaration order). Available rules: `is_email`, `is_url`, `is_alphanumeric`,
`min_len(n)`, `max_len(n)`, `is_uuid`.

## Generics

```varian
struct Box<T> { value: T }
fn identity<T>(x: T) -> T { return x }
```

Generics are **type-erased** — `<T>` is parsed for documentation/tooling purposes only,
with no runtime specialization or checking. A `Box<int>` and a `Box<string>` are the
exact same runtime representation.

## Enums and pattern matching

```varian
enum Option<T> {
    Some(T),
    None
}

let x = Option::Some(10)

match x {
    Option::Some(10) => print("got 10"),
    Option::None => print("got none")
}
```

## Structural typing (traits)

```varian
trait Shape {
    fn area(self) -> int
}

struct Rect { w: int, h: int }
impl Rect {
    fn area(self) -> int { return self.w * self.h }
}

struct Circle { r: int }
impl Circle {
    fn area(self) -> int { return self.r * self.r * 3 }
}

fn print_area(s: Shape) { print(s.area()) }

print_area(Rect { w: 10, h: 20 })   // 200
print_area(Circle { r: 5 })          // 75
```

A type satisfies a `trait` implicitly by having matching methods — there's no explicit
`impl Shape for Rect` to write.

## Error handling

```varian
// `?` propagates a `null` result up out of the current function
fn safe_div(a, b) {
    if b == 0 { return null }
    return a / b
}
fn try_div(a, b) {
    let result = safe_div(a, b)?   // short-circuits the whole function if null
    print(result)
}

// try/catch with an optionally-bound error value
try {
    throw("error!")
} catch e {
    print(e)
}
```

`?` here is null-propagation (closer to Swift's optional chaining than Rust's
`Result`-based `?`), not a typed-error-channel operator.

## Decorators

```varian
@cache
fn expensive(n) {
    print("computing")
    return n * 2
}

@retry(3)
fn flaky() {
    if should_fail { throw("failed") }
    return 42
}
```

`@cache` memoizes by argument values. `@retry(n)` restarts the function from the top up
to `n` times if it throws. `@validate`-style rules (`@is_email`, `@min_len(n)`, etc.) are
for struct fields, described above.

## Compile-time execution (`comptime`)

```varian
let x = comptime { 10 + 20 }   // x == 30, computed once at compile time
```

`comptime { ... }` runs its body immediately, in lexical position, during normal
compilation/execution — so it **can** call any function or use any global already
defined earlier in the program (including your own module functions), unlike a
pre-resolved-at-startup design. It cannot see anything defined *after* it, or anything
from an enclosing function's locals (it always runs with `enclosing = NULL`).

This is the basis of the comptime ORM in `vn_modules/db.vn` — see `docs/ZENITH.md`.

## FFI — calling C directly

```varian
@ffi("libm.so.6", "sqrt")
fn fast_sqrt(x: c_double) -> c_double

@ffi("libc.so.6", "abs")
fn fast_abs(x: c_int) -> c_int

print(fast_sqrt(9.0))   // 3.0
print(fast_abs(-42))    // 42
```

An `@ffi("path/to/lib.so", "symbol_name")`-decorated function declaration with no body
binds directly to a C shared library symbol via `libffi`, with FFI-specific parameter
types: `c_int`, `c_double`, `c_float`, `c_char`, `ptr`.

## Concurrency

See `docs/CONCURRENCY.md` for tasks, channels, and actors.

## Native modules & the Zenith framework

See `docs/STDLIB.md` for `math`/`string`/`io`/`sqlite`/`postgres`/`redis`/`http`/`auth`/
`validate`/`json`, and `docs/ZENITH.md` for the web framework and the comptime ORM.

## Tooling

See `docs/TOOLING.md` for `vn run`/`test`/`lint`/`fmt`.
