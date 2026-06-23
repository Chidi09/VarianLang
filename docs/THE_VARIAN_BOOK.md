# The Varian Programming Language Book

> **Ground truth.** Every statement in this book is backed by the Varian compiler and VM
> source code — `src/lexer.c`, `src/parser.c`, `src/vm.c`, `include/vm.h`, and the test
> suite under `tests/`.

---

## Table of Contents

- [Foreword](#foreword)
- [Introduction](#introduction)
- [1. Getting Started](#1-getting-started)
- [2. Programming a Guessing Game](#2-programming-a-guessing-game)
- [3. Common Programming Concepts](#3-common-programming-concepts)
- [4. Understanding Memory, Mutation, and Lifetimes](#4-understanding-memory-mutation-and-lifetimes)
- [5. Using Structs to Structure Related Data](#5-using-structs-to-structure-related-data)
- [6. Enums and Pattern Matching](#6-enums-and-pattern-matching)
- [7. Packages, Modules, and Scoping](#7-packages-modules-and-scoping)
- [8. Common Collections](#8-common-collections)
- [9. Error Handling](#9-error-handling)
- [10. Generic Types, Traits, and Lifetimes](#10-generic-types-traits-and-lifetimes)
- [11. Writing Automated Tests](#11-writing-automated-tests)
- [12. Building a Command Line Program](#12-building-a-command-line-program)
- [13. Functional Features: Iterators and Closures](#13-functional-features-iterators-and-closures)
- [14. Varian Tooling and Ecosystem](#14-varian-tooling-and-ecosystem)
- [15. Allocation, Smart Pointers, and GC Internals](#15-allocation-smart-pointers-and-gc-internals)
- [16. Cooperative Concurrency](#16-cooperative-concurrency)
- [17. Fundamentals of Asynchronous Programming](#17-fundamentals-of-asynchronous-programming)
- [18. Object-Oriented Patterns in Varian](#18-object-oriented-patterns-in-varian)
- [19. Pattern Matching and Guards](#19-pattern-matching-and-guards)
- [20. Advanced Features](#20-advanced-features)
- [21. Final Project: Web Programming with Zenith](#21-final-project-web-programming-with-zenith)
- [22. Appendices](#22-appendices)

---

## Foreword

Varian is a compiled, dynamically-typed systems programming language with a custom bytecode
VM. It targets three use cases:

1. **Network services** — a non-blocking, `io_uring`-powered HTTP framework (Zenith) with
   WebSocket, SSE, OpenAPI, and built-in SQLite/Postgres/Redis drivers.
2. **Agentic / glue code** — an embedded Python bridge (`python.run`) lets you call any
   Python library (boto3, numpy, etc.) without leaving Varian.
3. **Full-stack web applications** — Lumen (server-driven frontend framework) and Aurora
   (full-stack framework binding Lumen + Zenith) ship inside the `vn` binary.

The language is **dynamically typed at runtime** but accepts optional static type annotations
for linting and IDE support. The `vn lint` tool enforces them; the VM discards them before
execution.

The VM is a register-style stack machine with:
- **Computed-goto bytecode dispatch** (`src/vm.c:3640-3649`) — branch-predictor friendly,
  no central `switch` bottleneck.
- **Per-task bump arenas** (`src/vm.c:324-342`) — short-lived allocations (HTTP requests)
  bypass the GC entirely.
- **Mark-and-sweep GC** (`src/vm.c:707-960`) — tri-color collection at scheduler safepoints,
  runs between task ticks, never during allocation.
- **Single-threaded cooperative scheduler** (`src/vm.c:3262-3317`) — round-robins green-thread
  tasks; channels and actors provide safe, race-free communication.

---

## Introduction

### What Varian looks like

```varian
// A simple web server
let app = new_app()

app.get("/hello/:name", |req| {
    return Response {
        status: 200,
        body: "Hello, " + req.params.name + "!",
        content_type: "text/plain"
    }
}, "Say hello")

app.listen(3000)
```

### Key design decisions

| Decision | Choice | Why |
|---|---|---|
| **Typing** | Dynamic runtime + optional static annotations | Fast prototyping; `vn lint` provides safety |
| **Memory** | GC heap + per-task bump arenas | No borrow checker; predictable latency for request workloads |
| **Concurrency** | Cooperative green threads | Race-free by construction; no locks needed |
| **FFI** | libffi + `@ffi` decorator | Call any C library without wrapper code |
| **Stdlib** | Built into the binary | Zero `npm install`, zero supply-chain risk |
| **Web framework** | Built-in (Zenith) | Radix trie router, io_uring I/O, OpenAPI auto-generation |

---

## 1. Getting Started

### 1.1. Installation

Build the `vn` binary from source:

```bash
make clean
make -j$(nproc) CFLAGS="-std=gnu11 -O2 -DNDEBUG -Iinclude"
```

This produces a single native executable. No `node_modules`, no `pip install`, no `cargo install`.

```bash
./vn                    # Start the REPL
./vn run hello.vn       # Run a script
./vn test tests/        # Run the test suite
```

Dependencies: `build-essential`, `libcurl4-openssl-dev`, `libssl-dev`, `libsqlite3-dev`,
`libpq-dev`, `libhiredis-dev`, `libffi-dev`, `liburing-dev`.

### 1.2. Hello, World!

```varian
print("Hello, World!")
```

```bash
./vn hello.vn
```

`print` is a built-in global (`src/vm.c:3158`) that accepts any number of arguments,
converts each to its string representation, and writes to stdout.

### 1.3. Declaring dependencies

Packages are declared in `constellation.toml`:

```toml
[package]
name = "my-app"
version = "0.1.0"

[deps]
zenith = "latest"

[capabilities]
ffi = false
python = false
net = false
fs = false
```

`vn add zenith` adds the dependency and updates the manifest. `vn install` vendors all
dependencies into `vn_modules/`.

---

## 2. Programming a Guessing Game

Let's build a complete number-guessing game. This chapter introduces variables, input/output,
loops, conditionals, error handling, and random numbers.

### Setting up

```bash
mkdir guessing_game
cd guessing_game
touch main.vn
```

### Skeleton

```varian
// main.vn
print("Guess the number!")
```

### Storing values with `let`

```varian
let secret = 42       // immutable by convention (let is mutable)
let guess = ""        // string variable
```

Every binding is declared with `let`. Varian has no immutable-by-default distinction —
all `let` bindings can be reassigned. Type annotations are accepted for linting:

```varian
let secret: int = 42  // annotation discarded at runtime
```

### Reading user input

The `io` module provides `io.read_line()`:

```varian
print("Please input your guess.")
let input = io.read_line()
```

`io.read_line()` returns a `string`, or `null` if stdin is closed.

### Handling `null` return

```varian
if input == null {
    print("No input detected. Exiting.")
    return
}
```

### Printing variables

String concatenation uses `+`:

```varian
print("You guessed: " + input)
```

### Generating a random number

The native `math` module has no `random_int` function. Instead, use the `time` module for
seeding and `math` for modulo:

```varian
let seed = time.now_ms()
let secret = seed % 100 + 1   // random number between 1 and 100
```

### Parsing input and comparing

```varian
let guess = 0
try {
    guess = json_decode(input.trim())
} catch e {
    print("Please type a valid number!")
    return
}

if guess < secret {
    print("Too low!")
} else if guess > secret {
    print("Too high!")
} else {
    print("You win!")
}
```

### Looping until correct

```varian
loop {
    print("Please input your guess.")
    let input = io.read_line()
    if input == null {
        print("No input. Exiting.")
        break
    }

    let guess = 0
    try {
        guess = json_decode(input.trim())
    } catch e {
        print("Please type a valid number!")
        continue        // skip to next iteration
    }

    if guess < secret {
        print("Too low!")
    } else if guess > secret {
        print("Too high!")
    } else {
        print("You win!")
        break           // exit the loop
    }
}
```

### Complete program

```varian
let seed = time.now_ms()
let secret = seed % 100 + 1
print("Guess the number (1-100)!")

loop {
    print("Please input your guess.")
    let input = io.read_line()
    if input == null { break }

    let guess = 0
    try {
        guess = json_decode(input.trim())
    } catch e {
        print("Please type a valid number!")
        continue
    }

    if guess < secret {
        print("Too low!")
    } else if guess > secret {
        print("Too high!")
    } else {
        print("You win!")
        break
    }
}
```

### Testing

```bash
./vn main.vn
```

For automated testing, we'd write test assertions in a `tests/` directory (see Chapter 11).

---

## 3. Common Programming Concepts

### 3.1. Variables and Mutability

All variables are declared with `let` and are mutable by default:

```varian
let x = 5
x = 10              // works — can reassign
```

Type annotations are parsed and validated by `vn lint` but discarded before execution:

```varian
let price: float = 9.99      // annotation checked by linter, ignored by VM
let name: string = "Alice"
```

Multiple return values can be unpacked with comma-separated `let`:

```varian
let result, err = divide(10, 0)
```

Constants declared with `const` require an initializer and cannot be reassigned:

```varian
const PI = 3.14159   // compile-time constant
// PI = 3            // would cause a parse error
```

### 3.2. Data Types

The VM supports these value types (`include/vm.h:120-137`):

| Type | Example | Runtime representation |
|---|---|---|
| `nil` | `null` | `VAL_NIL` — singleton |
| `bool` | `true`, `false` | `VAL_BOOL` — 0 or 1 |
| `int` | `42`, `-7` | `VAL_INT` — `int64_t` |
| `float` | `3.14`, `1e10` | `VAL_FLOAT` — `double` |
| `string` | `"hello"` | `VAL_STRING` — UTF-8, immutable |
| `array` | `[1, 2, 3]` | `VAL_ARRAY` — COW dynamic slice |
| `tuple` | `(1, "a")` | `VAL_TUPLE` — fixed-size |
| `struct` | `User { id: 1 }` | `VAL_STRUCT` — GC-managed |
| `enum` | `Result::Ok(200)` | `VAL_ENUM` — tag + payload |
| `function` | `fn(x) x + 1` | `VAL_FUNCTION` / `VAL_CLOSURE` |
| `channel` | `task.channel(10)` | `VAL_CHANNEL` — bounded buffer |
| `task` | `task.spawn(fn)` | `VAL_TASK` — green thread |
| `actor` | `Counter.spawn()` | `VAL_ACTOR` — isolated process |

**Truthiness** (`src/vm.c:1059-1068`):

| Value | Truthy? |
|---|---|
| `null` | No |
| `false` | No |
| `0`, `0.0` | Yes (non-zero integers/floats are truthy) |
| `""` | No (empty string) |
| Everything else | Yes |

**Equality** (`src/vm.c:1070-1092`):
- Same-type comparison: `==` and `!=` compare values.
- Structs are **not** value-equal by default (comparison returns `false` for same-type
  structs with different fields).
- Enums: tag and payload values are compared recursively.
- Closures: compared by pointer identity (same closure object).

### 3.3. Functions

Declared with `fn`:

```varian
fn add(a, b) {
    return a + b
}
```

Type annotations are accepted by the parser and checked by `vn lint`:

```varian
fn add(a: int, b: int) -> int {
    return a + b
}
```

Arrow shorthand for single-expression bodies:

```varian
fn add(a, b) => a + b
```

Multiple return values:

```varian
fn divide(a, b) {
    if b == 0 {
        return null, "division by zero"
    }
    return a / b, null
}

let result, err = divide(10, 3)
if err != null {
    print("Error: " + err)
} else {
    print("Result: " + result)
}
```

Default argument values and rest parameters are not supported in the parser.

### 3.4. Comments

```varian
// Line comment

/*
 * Block comment
 */
```

Comments are preserved by `vn fmt` (`docs/TOOLING.md`).

### 3.5. Control Flow

**if / else if / else**:

```varian
if x > 10 {
    print("Big")
} else if x > 5 {
    print("Medium")
} else {
    print("Small")
}
```

**while**:

```varian
while x < 100 {
    x = x + 10
}
```

**for** (range iteration):

```varian
for i in 0..10 {
    print(i)     // 0, 1, 2, ..., 9
}

for item in items {
    print(item)
}
```

**loop** (infinite, with `break`/`continue`):

```varian
loop {
    let input = io.read_line()
    if input == null { break }
    if input == "" { continue }
    print(input)
}
```

---

## 4. Understanding Memory, Mutation, and Lifetimes

Varian uses a hybrid memory model: **structs are reference-typed** (GC-managed), while
**arrays use copy-on-write**. This is the single most important concept to understand
before writing Varian programs.

### 4.1. Struct reference semantics

Structs are **passed by reference**. When you assign a struct to a new variable or pass it
to a function, you share the same underlying object:

```varian
struct Point { x: int, y: int }
let a = Point { x: 1, y: 2 }
let b = a               // b references the SAME struct
b.x = 10                // modifies a.x as well!
print(a.x)              // 10 — mutation is visible through both references
```

This is identical to how JavaScript objects or Python dicts behave. The struct lives on
the GC heap (or, for short-lived requests, in a per-task bump arena).

### 4.2. Array copy-on-write semantics

Arrays use **copy-on-write (COW)**. Reading is shared; writing (via methods like `.push()`)
creates a new array:

```varian
let a = [1, 2, 3]
let b = a               // b shares the same buffer
let c = b.push(4)       // push CREATES a new array — b is unchanged
print(b)                // [1, 2, 3]
print(c)                // [1, 2, 3, 4]
```

For **in-place mutation**, use `.append()` instead:

```varian
let items = [1, 2, 3]
items.append(4)         // mutates the array in place (geometric growth)
print(items)            // [1, 2, 3, 4]
```

`.push()` → returns new array (COW). `.append()` → mutates in place.

### 4.3. In-place mutation and `self`

Inside `impl` blocks, methods take an explicit `self` parameter. Mutating fields on `self`
modifies the struct in place:

```varian
struct Counter { val: int }
impl Counter {
    fn increment(self) {
        self.val = self.val + 1   // in-place mutation of the struct
    }
}

let c = Counter { val: 0 }
c.increment()
print(c.val)            // 1 — the original struct was mutated
```

### 4.4. Per-task bump arenas

For high-throughput workloads (e.g., each HTTP request), Varian allocates a **64 KB bump
arena** per task (`src/vm.c:324-342`, `TASK_ARENA_SIZE = 64 * 1024`). New structs created
in a request handler are bump-allocated inside this arena — they never touch the GC heap.

When a struct **escapes** the arena (assigned to a global, sent through a channel, or
captured by a spawned task), the `escape_promote` write barrier (`src/vm.c:659`) deep-copies
it to the GC heap. This is automatic and invisible to the programmer.

### 4.5. Slicing

Arrays and strings support indexed access and slicing:

```varian
let arr = [10, 20, 30, 40]
print(arr[0])           // 10
print(arr[1..3])        // [20, 30]

let s = "hello"
print(s[0])             // "h" (string indexing returns a string)
print(s.substring(1, 4)) // "ell"
```

---

## 5. Using Structs to Structure Related Data

### 5.1. Defining and instantiating structs

```varian
struct User {
    id: int,
    name: string,
    email: string,
}

let user = User { id: 42, name: "Alice", email: "alice@example.com" }

// Field access
print(user.name)         // "Alice"

// Mutation
user.name = "Bob"
```

### 5.2. Structs with validation decorators

Decorators on struct fields register validation rules that run at construction:

```varian
struct SignupForm {
    @is_email email: string,
    @min_len(3) name: string,
    @max_len(100) name: string,
}
```

Supported decorators (`lib_validate.c`): `@is_email`, `@is_url`, `@is_alphanumeric`,
`@is_uuid`, `@min_len(n)`, `@max_len(n)`.

Validation is triggered by `vn lint` at development time, or by explicit calls to
`_validate.get_field()` and friends at runtime.

### 5.3. Anonymous / dynamic structs

For ad-hoc structs without a type declaration, use `http.create_struct`:

```varian
let obj = http.create_struct(
    ["name", "age", "email"],
    ["Alice", 30, "alice@example.com"]
)
print(obj.name)          // "Alice"
```

This is how Lumen and Aurora build data objects internally.

### 5.4. Methods and `impl` blocks

Behavior is attached to structs using `impl`:

```varian
impl User {
    fn display(self) {
        print(self.name + " <" + self.email + ">")
    }

    fn is_valid(self) -> bool {
        return self.name != "" and self.email != ""
    }
}

let user = User { id: 1, name: "Alice", email: "alice@example.com" }
user.display()           // "Alice <alice@example.com>"
```

Methods are dispatched via `BC_DISPATCH` / `BC_REGISTER_METHOD` at runtime
(`src/vm.c`). Unlike most OO languages, Varian resolves methods by **symbol name** at
runtime — there is no vtable.

---

## 6. Enums and Pattern Matching

### 6.1. Defining enums

Enums are sum types with optional payloads:

```varian
enum WebResult<T> {
    Success(T),
    Failure(string),
}

enum Color {
    Red,
    Green,
    Blue,
}
```

Generic type parameters (`<T>`) are accepted by the parser and discarded at runtime (no
monomorphization).

### 6.2. The `match` construct

`match` inspects an enum value and dispatches on the tag:

```varian
let res = WebResult::Success(200)

match res {
    WebResult::Success(code) => print("OK: " + code),
    WebResult::Failure(msg)  => print("Error: " + msg)
}
```

`match` also works on plain values:

```varian
match x {
    1 => print("one"),
    2 => print("two"),
    _ => print("other")
}
```

### 6.3. Concise control flow

**Null-coalescing** `??` — returns the left value if it's not null, otherwise the right:

```varian
let name = user?.name ?? "guest"
```

**Optional chaining** `?.` — safe member access:

```varian
let role = user?.profile?.role     // null if any intermediate is null
```

**Propagation** `expr?` — evaluates `expr`; if `null`, propagates `null` as the enclosing
function's return value:

```varian
fn get_admin_email(users, admin_id) {
    let user = users[admin_id]?       // returns null early if not found
    return user.email?                // returns null early if no email
}
```

---

## 7. Packages, Modules, and Scoping

### 7.1. The package manifest

Dependencies are declared in `constellation.toml`:

```toml
[package]
name = "my-service"
version = "0.1.0"
kind = "aurora"           # or omit for plain scripts

[deps]
zenith = "latest"
lumen-ui = "^1.2.0"

[capabilities]
ffi = false
python = false
net = true
fs = true
```

### 7.2. The `use` keyword

Modules are loaded with `use` (`src/parser.c:2538-2766`):

```varian
use "lib/helpers.vn"        // relative file path
use "auth"                   // vn_modules/auth/ directory
use "zenith" as zt          // namespaced import
```

Resolution order: literal file path → `vn_modules/<name>/` → `$VARIAN_HOME/vn_modules/<name>/`.

### 7.3. Namespaced imports

When `use "pkg" as alias` is used, the package is loaded into a namespaced struct:

```varian
use "db" as database
database.select("users").where("id", "=").build()
```

Functions starting with `_` are private and excluded from the export struct.

### 7.4. The concatenation prelude

All `.vn` files in `vn_modules/` are automatically concatenated as a prelude before your
entry file. This means `new_app()`, `lumen_mount()`, etc. are always in scope with no
imports needed.

### 7.5. Scope resolution

- **Globals**: declared with `let` at the top level. Visible everywhere after declaration.
- **Locals**: declared inside functions, visible only in their scope.
- **Upvalues**: captured by closures — see Chapter 13.

---

## 8. Common Collections

### 8.1. Arrays

Arrays are dynamic, type-erased lists:

```varian
let list = [1, 2, "three", true]
list.append(4)           // in-place mutation
let copy = list.push(5)  // returns new array (COW)

print(list.len())        // 5
print(list[0])           // index access
```

Array methods (from `src/lib_string.c`):
- `arr.len()` — element count
- `arr.push(x)` — returns new array with `x` appended (COW)
- `arr.append(x)` — mutates in place, geometric growth
- `arr.join(sep)` — concatenates string elements with separator

### 8.2. Strings

Strings are immutable UTF-8 sequences:

```varian
let s = "Hello, World!"
print(s.len())              // 13
print(s.upper())            // "HELLO, WORLD!"
print(s.lower())            // "hello, world!"
print(s.substring(0, 5))    // "Hello"
print(s.contains("World"))  // true
print(s.split(","))         // ["Hello", " World!"]
```

String methods:
- `s.len()`, `s.upper()`, `s.lower()`
- `s.substring(start)`, `s.substring(start, end)`
- `s.trim()`, `s.split(delimiter)`
- `s.starts_with(prefix)`, `s.ends_with(suffix)`
- `s.index_of(needle)`, `s.last_index_of(needle)`
- `s.contains(needle)`, `s.replace(old, new)`
- `s.code_at(index)`, `string.from_codes(array)`

String interpolation (lexer `TOKEN_INTERPOLATED_STRING`):

```varian
let name = "Alice"
let msg = "Hello, {name}!"       // "Hello, Alice!"
```

### 8.3. Dynamic structs as maps

Varian has no dedicated hash map type. Use `http.create_struct` for key-value mappings:

```varian
let config = http.create_struct(
    ["host", "port", "ssl"],
    ["localhost", 5432, true]
)
print(config.host)       // "localhost"
print(config.port)       // 5432

// Dynamic field access at runtime
let key = "host"
let val = _validate.get_field(config, key)
```

The `_validate` module provides `get_field`, `set_field`, `has_field`, and `get_keys` for
dynamic struct access.

---

## 9. Error Handling

### 9.1. `throw` — unrecoverable errors

```varian
throw("something went wrong")
throw(errors.make("ConfigError", "Missing API key", "Set the API_KEY env variable"))
```

A thrown value propagates up the call stack until caught or until it reaches the top level
(which aborts the program).

### 9.2. `try` / `catch` — recoverable errors

```varian
try {
    let val = risky_operation()
    print(val)
} catch e {
    print("Caught: " + e)
}
```

The `catch` variable receives the thrown value. Without a `catch` variable:

```varian
try {
    risky_operation()
} catch {
    print("Something went wrong")
}
```

### 9.3. The `?` operator — early propagation

`expr?` evaluates `expr`. If it's `null`, the operator **returns `null` immediately** from
the enclosing function:

```varian
fn lookup_user(id) {
    let row = db_query("SELECT * FROM users WHERE id = ?", [id])?
    return row
}
// If db_query returns null, lookup_user immediately returns null
```

### 9.4. The `errors` module

The native `errors` module (`src/lib_errors.c:52-110`) provides structured error handling:

```varian
// Create an error struct
let err = errors.make("ValidationError", "Email is invalid", "Use a valid email format")

// Inspect errors
print(errors.kind(err))          // "ValidationError"
print(errors.explain(err))       // "Email is invalid — Use a valid email format"

// Check error category
if errors.is(err, "ValidationError") {
    print("This is a validation error")
}
```

The VM produces errors with these categories: `UndefinedName`, `NoSuchField`,
`NoSuchMember`, `NoSuchMethod`, `DivByZero`, `IndexOutOfBounds`, `TypeMismatch`,
`WrongArgCount`, `InfiniteRecursion`, `ClosedChannel`, `UncaughtError`.

### 9.5. Catchable errors

These runtime errors are catchable with `try`/`catch` (`tests/error_catch_test.vn`):

```varian
try {
    let x = 1 / 0               // Division by zero
} catch e {
    print(errors.kind(e))       // "DivByZero"
}

try {
    let arr = [1, 2, 3]
    let x = arr[100]            // Index out of bounds
} catch e {
    print(errors.kind(e))       // "IndexOutOfBounds"
}

try {
    let s = Point { x: 1 }
    let y = s.z                  // No such field
} catch e {
    print(errors.kind(e))       // "NoSuchField"
}
```

### 9.6. Best practices

| Scenario | Mechanism |
|---|---|
| Expected absence (key not found, no input) | Return `null`, use `?.` and `??` |
| Expected failure with reason | Return `(result, error_string)` tuple |
| Unexpected / unrecoverable | `throw()` |
| Any of the above at system boundary | `try/catch` |

---

## 10. Generic Types, Traits, and Lifetimes

### 10.1. Type-erased generics

Generics are accepted by the parser but **fully erased at compile time** — there is zero
runtime overhead. They exist for `vn lint` and IDE support:

```varian
struct Holder<T> {
    val: T
}

fn identity<T>(x: T) -> T {
    return x
}
```

### 10.2. Implicit structural traits

Traits define a set of methods. Any struct that implements those methods implicitly
satisfies the trait — no explicit `impl Trait for Type` declaration needed:

```varian
trait Greeter {
    fn greet(self) -> string
}

struct Robot {}
impl Robot {
    fn greet(self) -> string {
        return "Beep boop"
    }
}

struct Human {}
impl Human {
    fn greet(self) -> string {
        return "Hello!"
    }
}

fn announce(g: Greeter) {
    print(g.greet())
}

let r = Robot {}
let h = Human {}
announce(r)             // "Beep boop"
announce(h)             // "Hello!"
```

### 10.3. Memory lifetimes

Varian has no borrow checker. Lifetimes are managed by the GC and per-task bump arenas:

- **GC heap** — structs live until the next mark-and-sweep cycle finds them unreachable.
- **Per-task bump arena** — structs allocated during a request handler live until the task
  finishes, then the arena is bulk-reclaimed (`arena_offset = 0`). If a struct escapes the
  task (assigned to a global or sent via channel), `escape_promote` copies it to the GC heap.

There is no way to manually free memory. The GC runs at safepoints between task ticks
(`src/vm.c:3255`), never during allocation.

---

## 11. Writing Automated Tests

### 11.1. Test syntax

Tests are declared with `test` blocks (`src/parser.c:1717-1749`):

```varian
test "math square root" {
    assert_eq(math.sqrt(16.0), 4.0)
}

test "string upper" {
    assert_eq("hello".upper(), "HELLO")
}

test "division by zero is catchable" {
    assert_throws(|| {
        let x = 1 / 0
    })
}
```

Assertion globals:
- `assert_eq(a, b)` — passes if `a == b`
- `assert_ne(a, b)` — passes if `a != b`
- `assert_throws(fn)` — passes if the zero-arg closure throws

### 11.2. Running tests

```bash
./vn test                     # Run all tests in tests/
./vn test tests/math_test.vn  # Run a specific file
./vn test --filter "square"   # Run only tests matching the substring
./vn test --timeout 30        # Set per-test timeout (default: 10s)
```

Each test runs in its own `Task` within a mini-scheduler. A passing test prints
`✅ PASS: "description"`. A failing test prints `❌ FAIL: "description"` plus the error.

### 11.3. Mocking native modules

Mock any native module function with `mock.intercept` (`src/lib_mock.c`):

```varian
// Replace math.sqrt with a fake
let original = mock.intercept("math", "sqrt", |x| {
    return 42.0
})

assert_eq(math.sqrt(16.0), 42.0)   // uses the fake

// Restore the original
mock.restore("math", "sqrt", original)
assert_eq(math.sqrt(16.0), 4.0)    // real implementation again
```

Useful for mocking database calls, HTTP requests, and time in tests:

```varian
test "database query with mock" {
    let original = mock.intercept("sqlite", "query", |conn, sql, params| {
        return [http.create_struct(["id", "name"], [1, "Mock User"])]
    })

    let rows = sqlite.query(null, "SELECT * FROM users", [])
    assert_eq(rows[0].name, "Mock User")

    mock.restore("sqlite", "query", original)
}
```

---

## 12. Building a Command Line Program

### 12.1. Accepting command line arguments

Command-line arguments are passed to the script after the script path:

```varian
// args.vn
print("Arguments: " + json_encode(_args))
```

```bash
./vn args.vn --input file.txt --verbose
```

The `_args` global (registered by the VM) is an array of strings.

### 12.2. Reading a file

```varian
let contents = io.read_text("input.txt")
if contents == null {
    print("File not found")
    return
}
print(contents)
```

Binary files:

```varian
let data = io.read_bytes("image.png")
io.write_bytes("copy.png", data)
```

### 12.3. File system operations

```varian
if io.exists("data/") {
    let files = io.list_dir("data/")
    for f in files {
        print(f)
    }
}

io.mkdir("output/")
io.write_text("output/result.txt", "Hello, file!")
io.delete("output/result.txt")
```

### 12.4. Environment variables

```varian
let host = env.get("HOST", "localhost")     // default if missing
let port_str = env.get("PORT", "8080")

// Crashes if missing:
let api_key = env.require("API_KEY")

// Load .env file:
env.load()
env.load("/etc/app/.env")
```

### 12.5. A complete CLI program

```varian
use "lib/config.vn"

fn main() {
    let args = _args
    if args.len() == 0 {
        print("Usage: app <file>")
        return
    }

    let path = args[0]
    let contents = io.read_text(path)
    if contents == null {
        print("Error: file not found: " + path)
        return
    }

    let lines = contents.split("\n")
    print("File: " + path + " (" + lines.len() + " lines)")
    for i in 0..lines.len() {
        print((i + 1) + ": " + lines[i])
    }
}

main()
```

---

## 13. Functional Features: Iterators and Closures

### 13.1. Closures

Closures are created with the pipe syntax `|params| body`:

```varian
let factor = 2
let double = |x| { return x * factor }
print(double(10))           // 20
```

Arrow-style single-expression body:

```varian
let triple = |x| x * 3
```

**Closures capture by value** (`tests/closure_capture_test.vn`). Each closure gets its own
copy of the captured variable:

```varian
let fns = []
for i in 0..5 {
    fns = fns.push(|| { return i })
}
print(fns[0]())             // 0 — each closure captures its own i
print(fns[1]())             // 1
```

### 13.2. Passing closures to functions

```varian
fn apply(f, x) {
    return f(x)
}

let result = apply(|x| { return x * 2 }, 10)
print(result)               // 20
```

### 13.3. Processing arrays

```varian
let nums = [1, 2, 3, 4, 5]
let doubled = []
for i in 0..nums.len() {
    doubled = doubled.push(nums[i] * 2)
}

// Or with a helper:
fn map(arr, f) {
    let result = []
    for i in 0..arr.len() {
        result = result.push(f(arr[i]))
    }
    return result
}
let tripled = map(nums, |x| x * 3)
```

### 13.4. Performance considerations

Simple `for` loops run at native VM speed via computed-goto dispatch
(`src/vm.c:3640-3649`). Closure-based iteration adds call overhead. For hot paths, prefer
direct loops.

---

## 14. Varian Tooling and Ecosystem

### 14.1. The `vn` CLI

| Command | Description |
|---|---|
| `vn` | Start interactive REPL |
| `vn run <file>` | Execute a script |
| `vn test [path]` | Run tests |
| `vn fmt [path]` | Format code (comment-preserving) |
| `vn lint [path]` | Static analysis (security, correctness, performance) |
| `vn build [file]` | Bundle → `.vnb` or native binary with `--release` |
| `vn lsp` | Start LSP server (VS Code, Neovim, Zed) |
| `vn new <name>` | Scaffold an Aurora full-stack project |
| `vn dev [dir] [port]` | Lumen dev server with live reload |
| `vn add <pkg>` | Add a package dependency |
| `vn install` | Install dependencies |
| `vn doctor` | Check project health |

### 14.2. Debug flags

```bash
VN_DEBUG_AST=1 ./vn run script.vn       # Print AST
VN_DEBUG_BYTECODE=1 ./vn run script.vn  # Print bytecode
```

### 14.3. Linting

`vn lint` walks the AST and flags:

- Unused variables
- Type annotation mismatches
- Concatenated SQL in strings (SQL injection risk)
- Hardcoded secrets
- N+1 query patterns (in Lumen templates)
- Missing error handling

### 14.4. The REPL

```bash
./vn
> let x = 5
> x * 10
50
> fn square(n) { return n * n }
> square(7)
49
```

---

## 15. Allocation, Smart Pointers, and GC Internals

This chapter describes how Varian manages memory at the VM level. Understanding this helps
you write predictable, high-performance code.

### 15.1. The mark-and-sweep GC

The VM uses a **tri-color mark-and-sweep** collector (`src/vm.c:707-960`):

1. **Mark roots**: globals, main function, all task stacks + frame functions, dispatch table
   entries (`gc_mark_roots`, `vm.c:740`).
2. **Trace gray set**: walk into struct fields, array elements, tuples, enums, function
   constants, closure captured values, actor state/inbox, channel buffers (`gc_trace`,
   `vm.c:807`).
3. **Sweep unmarked**: free all unmarked objects, remove unmarked strings from intern table
   (`gc_sweep`, `vm.c:908`).
4. **Resize**: `next_gc_size` doubles after each collection.

The GC runs **only at safepoints** — between ticks of the round-robin scheduler
(`src/vm.c:3255`), never during an allocation. This eliminates "stop-the-world" latency at
unpredictable moments.

### 15.2. Heap object reference counts

Every heap object (`ObjHeader`, `include/vm.h`) has an `obj_refs` field used by the GC
for reachability analysis. The collector counts references from roots and traces
transitive references — this is not a reference-counting collector, but the counter helps
determine reachability during sweep.

### 15.3. Per-task bump arenas

For short-lived allocations (typical in HTTP request handlers), Varian uses a **64 KB
bump arena** per task (`src/vm.c:324-342`):

```c
// src/vm.c — per-task bump arena
void *task_arena_alloc(Task *t, size_t size) {
    if (t && t->use_arena && t->arena_base) {
        size_t aligned = (size + 7) & ~7;          // 8-byte align
        if (t->arena_offset + aligned <= TASK_ARENA_SIZE) {
            void *ptr = t->arena_base + t->arena_offset;
            t->arena_offset += aligned;
            memset(ptr, 0, size);
            return ptr;
        }
    }
    return malloc(size);                           // fallback to heap
}
```

- Arena is enabled per-task via `task_arena_enable(t)`.
- Structs allocated in the arena are **not** linked into `vm->objects` — they bypass the GC
  entirely.
- On task completion, `arena_offset` is reset to 0 — bulk reclamation with zero cost.

### 15.4. The `escape_promote` write barrier

When an arena-backed struct escapes the task (assigned to a global, sent through a channel,
or captured by a spawned task), the `escape_promote` function (`src/vm.c:659`) deep-copies
it to the GC heap:

```c
// src/vm.c — escape_promote deep-copies arena structs to GC heap
Value escape_promote(VM *vm, Value val) {
    if (!is_in_arena(vm, val)) return val;   // not in arena, no copy needed
    // Deep copy the struct and all its references to the GC heap
    ...
}
```

This is automatic and invisible. The only observable effect: after escaping, the struct is
now GC-managed instead of arena-managed.

### 15.5. Object pooling (task free-list)

Dead tasks are not freed — they're pushed to a **free-list** (`src/vm.c:2435-2467`) and
recycled on the next `task.spawn()`:

```c
Task *task_new(VM *vm) {
    Task *t = vm->free_tasks;
    if (t) {
        vm->free_tasks = (Task *)t->http_response_ssl;  // pop from free list
        memset(t, 0, sizeof(Task));
        return t;
    }
    return (Task *)calloc(1, sizeof(Task));
}
```

### 15.6. Reference cycles

The mark-and-sweep collector handles reference cycles that pure reference counting cannot.
If struct A references struct B and B references A, and both are unreachable from roots,
both will be freed in the sweep phase.

---

## 16. Cooperative Concurrency

### 16.1. Green-thread tasks

Tasks are cooperatively scheduled green threads (`src/vm.c:3262-3317`). Spawn a task with
`task.spawn`:

```varian
fn worker(id) {
    print("Task " + id + " starting")
    task.yield()
    print("Task " + id + " resuming")
}

let t = task.spawn(worker, [1])
let t2 = task.spawn(worker, [2])

await t     // wait for task to complete
await t2
```

The scheduler round-robins tasks sequentially. Only **one task runs at a time** — there
is no parallelism, which means **no race conditions on shared state**.

### 16.2. Yielding

A task yields the scheduler voluntarily:
- Explicitly: `task.yield()`
- On channel operations: `ch <- val` (send), `<- ch` (receive)
- On `task.sleep(ms)`: non-blocking sleep
- On `await`: wait for another task to complete

```varian
loop {
    let msg = <- ch
    if msg == null { break }
    process(msg)
    task.yield()    // let other tasks run
}
```

### 16.3. Channels

Channels are bounded, circular buffers for task communication:

```varian
let ch = task.channel(10)       // capacity 10

// Send (yields if full)
ch <- "hello"
let result = 42
ch <- result

// Receive (yields if empty)
let val = <- ch

// Try receive (non-blocking)
let val = task.try_receive(ch)

// Close
task.close(ch)
```

**Automated backpressure**: If the channel is full, the sender yields until space is
available. If the channel is empty, the receiver yields until data arrives.

```varian
fn producer(ch) {
    for i in 0..100 {
        ch <- i             // yields if buffer full
    }
    task.close(ch)
}

fn consumer(ch) {
    loop {
        let val = <- ch     // yields if buffer empty
        if val == null { break }
        print("Got: " + val)
    }
}

let ch = task.channel(10)
task.spawn(producer, [ch])
task.spawn(consumer, [ch])
```

### 16.4. Stateful actors

Actors isolate state behind a message-passing interface. An actor has internal fields and
methods — calling a method sends a message to the actor's mailbox:

```varian
actor Counter {
    val: int = 0,

    fn increment(self) {
        self.val = self.val + 1
    }

    fn get(self) -> int {
        return self.val
    }
}

let c = Counter.spawn()
c.increment()
c.increment()
print(c.get())              // 2
```

**How actors work internally** (`src/vm.c:2936-3051`):

1. `Counter.spawn()` creates an `ObjActor` with an inbox channel (capacity 64).
2. A background `loop_task` (with `is_actor_loop = true`) polls the inbox.
3. `c.increment()` sends a `(method_name, args, reply_ch)` tuple to the inbox.
4. The loop task runs the method, sends the result back via `reply_ch`.
5. The calling task yields until the result arrives.

Actors provide **deterministic isolation** — state is never shared, only messages. Since
the VM is single-threaded, actor messages are processed atomically: one message per
scheduler tick.

---

## 17. Fundamentals of Asynchronous Programming

### 17.1. The cooperative scheduling loop

The core scheduler (`src/vm.c:3248-3360`) iterates over all tasks:

```
while tasks remain:
    for each task t:
        if t is dead: continue
        t.yielded = false
        task_run(vm, t)             // runs until yield or completion
        if all tasks idle:
            nanosleep(1ms)          // idle backoff
    gc_collect()                     // safepoint
```

Because scheduling is cooperative, a tight loop that never yields will block all other
tasks. Always include a `task.yield()` or channel operation in long-running loops.

### 17.2. Spawning concurrent tasks

```varian
fn fetch_url(url, ch) {
    let resp = http.get(url)
    ch <- resp
}

let ch = task.channel(10)
task.spawn(fetch_url, ["https://api.example.com/data", ch])
task.spawn(fetch_url, ["https://api.example.com/other", ch])

let result1 = <- ch
let result2 = <- ch
```

### 17.3. Worker pools

The `queue.vn` module provides a built-in `WorkerPool`:

```varian
// Spawn a pool of 4 workers
let pool = WorkerPool { ch: task.channel(100), count: 0, workers: 0, stopped: false }
pool.spawn(4)

// Submit work
pool.submit(|| {
    print("Working...")
    return 42
})

// Stop the pool
pool.stop()
```

### 17.4. Cron jobs

```varian
cron(5000, || {                // every 5 seconds
    print("Tick")
})
```

The `cron` function spawns a task that sleeps for `interval_ms`, runs the handler, and
loops.

### 17.5. Channels for streaming

```varian
fn stream_lines(path, ch) {
    let contents = io.read_text(path)
    if contents == null {
        task.close(ch)
        return
    }
    let lines = contents.split("\n")
    for i in 0..lines.len() {
        ch <- lines[i]          // stream one line at a time
    }
    task.close(ch)
}

let ch = task.channel(10)
task.spawn(stream_lines, ["large_file.txt", ch])

loop {
    let line = <- ch
    if line == null { break }
    process_line(line)
}
```

---

## 18. Object-Oriented Patterns in Varian

### 18.1. Encapsulation with structs + impl

Varian has no access modifiers (no `private`/`public`). Encapsulation is achieved by
convention: internal helper functions are prefixed with `_`:

```varian
fn _validate_email(email) {
    return email.contains("@")
}

struct User {
    name: string,
    email: string,
}

impl User {
    fn is_valid(self) -> bool {
        return _validate_email(self.email)
    }
}
```

### 18.2. Polymorphism via structural traits

Trait objects enable polymorphic dispatch without inheritance:

```varian
trait Formatter {
    fn format(self, val) -> string
}

struct JsonFormatter {}
impl Formatter {
    fn format(self, val) -> string {
        return json_encode(val)
    }
}

struct PlainFormatter {}
impl Formatter {
    fn format(self, val) -> string {
        return "" + val
    }
}

fn print_formatted(f: Formatter, val) {
    print(f.format(val))
}

let json_f = JsonFormatter {}
let plain_f = PlainFormatter {}
print_formatted(json_f, [1, 2, 3])   // "[1,2,3]"
print_formatted(plain_f, [1, 2, 3])  // "1,2,3"
```

### 18.3. State and behavior separation

Structs hold data; `impl` blocks define behavior. Multiple `impl` blocks for the same
struct are allowed:

```varian
struct Point { x: int, y: int }

impl Point {
    fn distance_from_origin(self) -> float {
        return math.sqrt(self.x * self.x + self.y * self.y)
    }
}

impl Point {
    fn to_string(self) -> string {
        return "(" + self.x + ", " + self.y + ")"
    }
}
```

---

## 19. Pattern Matching and Guards

### 19.1. Where `match` applies

`match` works on:
- Enum variants (with payload unpacking)
- Integer constants
- String constants
- Ranges
- Wildcards (`_`)

```varian
enum HttpStatus {
    Ok(int),
    NotFound,
    Error(string),
}

fn describe(status) -> string {
    return match status {
        HttpStatus::Ok(code) if code < 300 => "Success",
        HttpStatus::NotFound => "Not Found",
        HttpStatus::Error(msg) => "Error: " + msg,
        _ => "Unknown"
    }
}
```

### 19.2. Match guards

Guards are `if` conditions attached to match arms:

```varian
match x {
    n if n < 10 => print("small"),
    n if n < 100 => print("medium"),
    _ => print("large")
}
```

### 19.3. Pattern syntax

| Pattern | Example | Matches |
|---|---|---|
| Constant | `42` | The integer 42 |
| String | `"hello"` | The string "hello" |
| Range | `0..10` | Integers 0 through 9 |
| Wildcard | `_` | Any value |
| Binding | `x` | Any value, bound to name |
| Enum | `Result::Ok(v)` | Enum variant with payload bound to `v` |
| Guard | `x if x > 0` | Binding with condition |

---

## 20. Advanced Features

### 20.1. Direct C FFI via `@ffi`

Call any C shared library function without wrapper code:

```varian
@ffi("libm.so.6", "sqrt")
fn c_sqrt(x: c_double) -> c_double

@ffi("libpthread.so.0", "pthread_self")
fn pthread_self() -> ptr

let result = c_sqrt(16.0)   // 4.0
```

The `@ffi` decorator (`src/vm.c:2546-2586`) registers the library + symbol in `ffi_decls[]`.
At VM init, the library is loaded with `dlopen` and the symbol resolved with `dlsym`. At
runtime, `BC_FFI_CALL` invokes the function via libffi.

FFI parameter types: `c_int`, `c_double`, `c_float`, `c_char`, `ptr`.

### 20.2. The `@cache` decorator

Caches the return value of a function based on its arguments:

```varian
@cache
fn expensive_calc(n) {
    // ... expensive computation ...
    return result
}

expensive_calc(42)   // computes and caches
expensive_calc(42)   // returns cached result
```

The cache uses `cache_key_hash(fn, args)` and `cache_map` (`src/vm.c:4196-4209`).

### 20.3. The `@retry` decorator

Retries a function up to N times on failure:

```varian
@retry(3)
fn fetch_data(url) {
    let resp = http.get(url)
    if resp == null { throw("network error") }
    return resp
}

fetch_data("https://example.com/api")   // retries up to 3 times
```

`@retry(n)`: on `throw` or native error, decrements the retry counter and restarts the
function from the beginning (`src/vm.c:4151-4177`).

### 20.4. Compile-time evaluation with `comptime`

`comptime { ... }` executes the block at compile time and inlines the result:

```varian
let sql = comptime {
    select("users")
        .fields(["id", "name", "email"])
        .where("id", "=")
        .build()
}
// sql.sql == "SELECT id, name, email FROM users WHERE id = ?"
// sql.param_count == 1
```

How it works (`src/vm.c:2316-2366`):
1. The body is compiled into a temporary `ObjFunction`.
2. It's executed immediately in a synchronous `task_run()`.
3. The result value is stored back into the outer function's constant table.
4. The body runs at **lexical position** — it can see previously defined globals but not
   local variables.

### 20.5. The Python bridge

Call any Python library via `python.run`:

```varian
python.run("import json")
let data = python.run("json.dumps({'hello': 'world'})")
```

For external SDK access (S3, etc.):

```varian
fn upload_to_s3(bucket, key, data) {
    python.run("
import boto3
s3 = boto3.client('s3')
s3.put_object(Bucket='" + bucket + "', Key='" + key + "', Body=" + json_encode(data) + ")
    ")
}
```

---

## 21. Final Project: Web Programming with Zenith

This chapter builds a complete web application using Zenith (HTTP framework) and Lumen
(frontend framework) within an Aurora project structure.

### 21.1. Scaffolding

```bash
vn new task-tracker
cd task-tracker
```

This creates:
```
task-tracker/
  main.vn                    # Zenith API server
  pages/                     # Lumen frontend
    index.lumen
    components/
  lib/                       # Shared modules
    config.vn
  public/                    # Static assets (favicons, manifest)
  constellation.toml         # kind = "aurora"
```

### 21.2. The API server

```varian
// main.vn — Zenith HTTP server
let app = new_app()
app.title = "Task Tracker"

// Middleware
app.add_middleware(logging_middleware)

// Routes
app.get("/api/tasks", |req| {
    let rows = sqlite.query(conn, "SELECT * FROM tasks ORDER BY id DESC", [])
    return json_response(rows, 200)
}, "List all tasks", null)

app.post("/api/tasks", |req| {
    let title = req.json.title
    if title == null { return json_response({ error: "Title required" }, 400) }
    sqlite.query(conn, "INSERT INTO tasks (title) VALUES (?)", [title])
    return json_response({ ok: true }, 201)
}, "Create a task", null)

app.delete("/api/tasks/:id", |req| {
    sqlite.query(conn, "DELETE FROM tasks WHERE id = ?", [req.params.id])
    return json_response({ ok: true }, 200)
}, "Delete a task", null)

// OpenAPI docs
app.enable_docs("/docs")

app.listen(8080)
```

### 21.3. Middleware

```varian
fn logging_middleware(req, next) {
    let start = time.now_ms()
    let resp = next(req)
    let elapsed = time.now_ms() - start
    print(req.method + " " + req.path + " → " + resp.status + " (" + elapsed + "ms)")
    return resp
}

// Security middleware from shield.vn
app.add_middleware(cors(["*"], ["GET", "POST", "PUT", "DELETE"], ["*"]))
app.add_middleware(rate_limit(100, 60000))
```

### 21.4. Database with comptime ORM

```varian
let conn = sqlite.connect("tasks.db")

// Compile query at build time — zero runtime SQL construction
let select_all = comptime {
    select("tasks")
        .fields(["id", "title", "done", "created_at"])
        .build()
}
// select_all.sql == "SELECT id, title, done, created_at FROM tasks"
// select_all.param_count == 0

let insert_task = comptime {
    select("tasks")          // insert uses select builder for parameter shape
        .fields(["title"])
        .build()
}

// Bind and execute
let rows = run_sqlite(bind(select_all, []), conn)
sqlite.query(conn, "INSERT INTO tasks (title) VALUES (?)", ["Buy groceries"])
```

### 21.5. Lumen frontend

```html
<!-- pages/index.lumen -->
<template>
<Page>
  <Container size="md">
    <Stack gap="4">
      <Heading size="3xl">Task Tracker</Heading>

      <Row gap="2">
        <input class="input" id="new-task" placeholder="Add a task..." />
        <Button @click="addTask">Add</Button>
      </Row>

      <Stack gap="2">
        <Card @for="task in tasks">
          <Row justify="space-between" align="center">
            <Text>{{ task.title }}</Text>
            <Button variant="ghost" @click="deleteTask" data-id="{{ task.id }}">✕</Button>
          </Row>
        </Card>
      </Stack>

      <Text muted>You have {{ tasks.len() }} tasks.</Text>
    </Stack>
  </Container>
</Page>
</template>

<script>
fn state() {
    return { tasks: [], input: "" }
}

fn addTask(s, v) {
    let title = _validate.get_field(document, "getElementById")("new-task").value
    http.post("/api/tasks", json_encode({ title: title }), "")
    let rows = http.get("/api/tasks")
    return s.set("tasks", json_decode(rows))
}

fn deleteTask(s, v) {
    let id = v.getAttribute("data-id")
    http.delete("/api/tasks/" + id)
    let rows = http.get("/api/tasks")
    return s.set("tasks", json_decode(rows))
}
</script>
```

### 21.6. Sessions (stateless JWT)

```varian
let secret = "change-me-in-production"

// Set session data
let resp = session_set(
    json_response({ ok: true }, 200),
    http.create_struct(["uid", "role"], [1, "admin"]),
    secret,
    null
)

// Read session data
let session = session_get(req, secret)
if session == null {
    return json_response({ error: "Not authenticated" }, 401)
}
print(session.uid)      // 1

// Clear session
let resp = session_clear(json_response({ ok: true }, 200))
```

### 21.7. File storage

```varian
let store = new_storage("uploads/")

// Write
store.put("avatar-1.png", io.read_bytes("local-avatar.png"))

// Read
let bytes = store.get("avatar-1.png")

// List
store.list()

// Delete
store.delete("avatar-1.png")
```

### 21.8. Background jobs

```varian
// Worker pool for email
let pool = WorkerPool { ch: task.channel(100), count: 0, workers: 0, stopped: false }
pool.spawn(4)

pool.submit(|| {
    send_smtp("localhost", 1025, "noreply@example.com",
              "user@example.com", "Welcome!", "Thanks for signing up!")
})

// Cron job for maintenance
cron(3600000, || {          // every hour
    sqlite.query(conn, "DELETE FROM tasks WHERE done = 1 AND created_at < ?",
                 [time.now_ms() - 86400000])
})
```

### 21.9. Testing

```varian
test "GET /api/tasks returns array" {
    let req = http.test_request("GET", "/api/tasks", "")
    let resp = app.handle(req)
    assert_eq(resp.status, 200)
    let body = json_decode(resp.body)
    assert_ne(body, null)
}
```

---

## 22. Appendices

### 22.1. Appendix A: Keywords

```
let, const, fn, return, if, else, while, for, in, loop,
match, case, struct, enum, actor, impl, trait, type, use,
pub, mut, async, await, break, continue, try, catch,
assert, test, comptime, true, false, null, as,
bool, int, float, string, byte, void,
and, or, not           (word-form logical operators)
```

### 22.2. Appendix B: Operators (by precedence)

Precedence from lowest to highest (`src/parser.c`):

| Level | Operators | Associativity |
|---|---|---|
| Range | `..` | Left |
| Assignment | `=`, `+=`, `-=`, `*=`, `/=`, `<-` | Right |
| Nil coalesce | `??` | Left |
| Logical OR | `\|\|`, `or` | Left |
| Logical AND | `&&`, `and` | Left |
| Equality | `==`, `!=` | Left |
| Comparison | `<`, `>`, `<=`, `>=` | Left |
| Bit OR | `\|` | Left |
| Bit XOR | `^` | Left |
| Bit AND | `&` | Left |
| Term | `+`, `-` | Left |
| Factor | `*`, `/`, `%` | Left |
| Unary | `-`, `!`, `not`, `~`, `await`, `<-` (receive) | Right |
| Call | `()`, `[]`, `.`, `?.`, `expr?`, `Type{}` | Left |

### 22.3. Appendix C: Field decorators

| Decorator | Description |
|---|---|
| `@is_email` | Validates email format |
| `@is_url` | Validates URL format |
| `@is_alphanumeric` | Validates alphanumeric only |
| `@is_uuid` | Validates UUID format |
| `@min_len(n)` | Minimum string length |
| `@max_len(n)` | Maximum string length |

### 22.4. Appendix D: Native modules

| Module | Functions |
|---|---|
| `math` | `sin`, `cos`, `sqrt`, `abs`, `floor`, `ceil`, `bit_and`, `bit_or`, `bit_xor` |
| `io` | `read_text`, `write_text`, `read_bytes`, `write_bytes`, `exists`, `mkdir`, `delete`, `list_dir` |
| `task` | `spawn`, `yield`, `sleep`, `channel`, `close`, `try_receive`, `id` |
| `time` | `now_ms`, `now_iso8601` |
| `env` | `get`, `require`, `load` |
| `errors` | `make`, `kind`, `is`, `explain` |
| `http` | `get`, `post`, `serve`, `serve_tls`, `create_struct`, `write_socket`, `read_socket`, `close_socket`, `test_request` |
| `auth` | `hash_sha256`, `sign_jwt`, `verify_jwt`, `hash_password`, `verify_password`, `generate_token`, `constant_time_eq`, `sha1_base64` |
| `sqlite` | `connect`, `query`, `close` |
| `postgres` | `connect`, `query`, `close` |
| `redis` | `connect`, `cmd`, `close` |
| `smtp` | `send` |
| `regex` | `test`, `match`, `groups`, `find_all`, `replace` |
| `validate` | `is_email`, `is_url`, `is_alphanumeric`, `is_uuid`, `min_len`, `max_len` |
| `mock` | `intercept`, `restore` |
| `python` | `run` |

### 22.5. Appendix E: Varian modules (`.vn` prelude)

All files in `vn_modules/` are auto-loaded at global scope:

| Module | Provides |
|---|---|
| `zenith.vn` | `new_app()`, routing, middleware, WebSocket, SSE, OpenAPI, sessions, templates |
| `lumen.vn` | `lumen_component`, `lumen_mount`, `lumen_store`, `lumen_resource`, `lumen_form`, 28 UI components |
| `db.vn` | `select()`, `bind()`, `run_sqlite()`, `run_postgres()`, `QueryBuilder` |
| `auth.vn` | `zenith_auth.jwt()`, `zenith_auth.session_store()`, `zenith_auth.session()` |
| `shield.vn` | `cors()`, `csrf()`, `rate_limit()`, `rate_limit_redis()` |
| `queue.vn` | `WorkerPool`, `cron()` |
| `mail.vn` | `send_smtp()`, `send_resend()` |
| `storage.vn` | `new_storage()`, `.put()`, `.get()`, `.delete()`, `.list()` |
| `observe.vn` | `Logger.info()`, `.warn()`, `.error()`, `.info_with()`, `metrics_handler()` |
| `validate.vn` | `Validator`, `ObjectValidator` — chained validation |

### 22.6. Appendix F: Error categories

Errors thrown by the VM have these `kind` values (from `src/lib_errors.c`):

| Category | Trigger |
|---|---|
| `UndefinedName` | Reference to an undefined variable |
| `NoSuchField` | Struct field access on missing field |
| `NoSuchMember` | Method call on non-existent method |
| `NoSuchMethod` | Dispatch to unimplemented method |
| `DivByZero` | Integer division by zero |
| `IndexOutOfBounds` | Array index outside valid range |
| `TypeMismatch` | Operation on incompatible types |
| `WrongArgCount` | Function call with wrong argument count |
| `InfiniteRecursion` | Stack overflow detected |
| `ClosedChannel` | Send on closed channel |
| `UncaughtError` | Error reached top level |

### 22.7. Appendix G: The `owo` syntax color scheme

The VS Code extension applies the `owo` color scheme:

| Element | Color | Hex |
|---|---|---|
| Keywords | Pastel Pink | `#ff9ebb` |
| Operators | Warm Coral | `#ff6f69` |
| Functions | Soft Lavender | `#d0bdf4` |
| Types | Mint Green | `#a8e6cf` |
| Strings | Creamy Peach | `#ffd3b6` |
| Numbers | Butter Yellow | `#fffaaa` |
| Comments | Slate Gray | `#8892b0` |
| Decorators | Neon Purple | `#b388ff` |
