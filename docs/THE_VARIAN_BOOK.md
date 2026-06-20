# The Varian Programming Language Book

This book is a comprehensive, chapter-by-chapter guide to the **Varian Programming Language**, a modern, fast, agentic programming language designed to bridge native performance with universal foreign function interoperability and cooperative concurrency.

---

## Table of Contents

*   [Foreword](#foreword)
*   [Introduction](#introduction)
*   [1. Getting Started](#1-getting-started)
    *   [1.1. Installation](#11-installation)
    *   [1.2. Hello, World!](#12-hello-world)
    *   [1.3. Tooling and Package Scaffolding](#13-tooling-and-package-scaffolding)
*   [2. Programming a Guessing Game](#2-programming-a-guessing-game)
*   [3. Common Programming Concepts](#3-common-programming-concepts)
    *   [3.1. Variables and Mutability](#31-variables-and-mutability)
    *   [3.2. Data Types](#32-data-types)
    *   [3.3. Functions](#33-functions)
    *   [3.4. Comments](#34-comments)
    *   [3.5. Control Flow](#35-control-flow)
*   [4. Understanding Memory & Mutation](#4-understanding-memory--mutation)
    *   [4.1. Struct Mutation (In-Place)](#41-struct-mutation-in-place)
    *   [4.2. Array Mutation (Copy-On-Write)](#42-array-mutation-copy-on-write)
    *   [4.3. The Garbage Collector](#43-the-garbage-collector)
*   [5. Structs and Methods](#5-structs-and-methods)
    *   [5.1. Defining and Instantiating Structs](#51-defining-and-instantiating-structs)
    *   [5.2. Struct Field Decorators & Validation](#52-struct-field-decorators--validation)
    *   [5.3. Methods and `impl` Blocks](#53-methods-and-impl-blocks)
*   [6. Enums and Pattern Matching](#6-enums-and-pattern-matching)
    *   [6.1. Defining an Enum](#61-defining-an-enum)
    *   [6.2. Match Control Flow Construct](#62-match-control-flow-construct)
*   [7. Packages, Modules, and the Prelude](#7-packages-modules-and-the-prelude)
    *   [7.1. Package Structure with `varian.pkg`](#71-package-structure-with-varianpkg)
    *   [7.2. Module Loading and the Concatenation Prelude](#72-module-loading-and-the-concatenation-prelude)
*   [8. Common Collections](#8-common-collections)
    *   [8.1. Storing Sequential Data with Arrays](#81-storing-sequential-data-with-arrays)
    *   [8.2. Tuples](#82-tuples)
*   [9. Error Handling](#9-error-handling)
    *   [9.1. Null-Propagation with the `?` Operator](#91-null-propagation-with-the--operator)
    *   [9.2. Recoverable Errors with `try/catch/throw`](#92-recoverable-errors-with-trycatchthrow)
*   [10. Generic Types, Traits, and Lifetimes](#10-generic-types-traits-and-lifetimes)
    *   [10.1. Type-Erased Generics](#101-type-erased-generics)
    *   [10.2. Implicit Structural Traits](#102-implicit-structural-traits)
    *   [10.3. Lifetimes](#103-lifetimes)
*   [11. Writing Automated Tests](#11-writing-automated-tests)
    *   [11.1. How to Write Tests](#111-how-to-write-tests)
    *   [11.2. Mocking and Intercepting Native Modules](#112-mocking-and-intercepting-native-modules)
*   [12. Building a Command Line Program](#12-building-a-command-line-program)
*   [13. Functional Features: Closures and Lambdas](#13-functional-features-closures-and-lambdas)
*   [14. Package Scaffolding & Foreign Wrappers](#14-package-scaffolding--foreign-wrappers)
*   [15. Garbage Collection & Object Lifecycle](#15-garbage-collection--object-lifecycle)
*   [16. Cooperative Concurrency](#16-cooperative-concurrency)
    *   [16.1. Spawning Tasks](#161-spawning-tasks)
    *   [16.2. Channels with Automated Backpressure](#162-channels-with-automated-backpressure)
    *   [16.3. The Actor Model](#163-the-actor-model)
*   [17. Asynchronous Programming Fundamentals](#17-asynchronous-programming-fundamentals)
    *   [17.1. Cooperative Green-Thread Scheduling](#171-cooperative-green-thread-scheduling)
    *   [17.2. Worker Pools and Background Jobs](#172-worker-pools-and-background-jobs)
*   [18. Object-Oriented Patterns in Varian](#18-object-oriented-patterns-in-varian)
*   [19. Pattern Matching Syntax and Guards](#19-pattern-matching-syntax-and-guards)
*   [20. Advanced Features](#20-advanced-features)
    *   [20.1. Direct C FFI via `libffi`](#201-direct-c-ffi-via-libffi)
    *   [20.2. The Embedded Python Bridge](#202-the-embedded-python-bridge)
    *   [20.3. Compile-Time Evaluation with `comptime`](#203-compile-time-evaluation-with-comptime)
*   [21. Final Project: Web Programming with Zenith](#21-final-project-web-programming-with-zenith)
    *   [21.1. Building a Web Application](#211-building-a-web-application)
    *   [21.2. Middleware Chains](#212-middleware-chains)
    *   [21.3. Comptime Database ORM](#213-comptime-database-orm)
*   [22. Appendices](#22-appendices)
    *   [22.1. Appendix A: Keywords and Special Identifiers](#221-appendix-a-keywords-and-special-identifiers)
    *   [22.2. Appendix B: Language Tooling (`vn fmt` and `vn lint`)](#222-appendix-b-language-tooling-vn-fmt-and-vn-lint)

---

## Foreword

Varian is built for systems and services where performance, rapid integration, and memory safety are paramount. It represents a synthesis of three core paradigms:
1.  **Direct Native Control**: Direct shared-library bindings without the weight of conventional wrapping code.
2.  **Universal Foreign Bridges**: Zero-setup calling of Python modules natively.
3.  **Cooperative Green Threads**: Micro-tasks and actor communication built directly on a lightweight VM.

---

## Introduction

Varian is dynamically typed at runtime but incorporates static type syntax used for editor validation, documentation generation, and static linting via the [vn lint](file:///root/dev/VarianLang/docs/TOOLING.md#L66) command. Execution runs on a custom, optimized VM using a bump-allocated struct arena and label-as-values computed-goto dispatch.

---

## 1. Getting Started

### 1.1. Installation

To build the Varian runtime (`vn`) from source:
```bash
make clean
make -j8 CFLAGS="-Wall -Wextra -std=gnu11 -O2 -DNDEBUG -Iinclude"
```
Verify the installation by querying the interactive REPL:
```bash
./vn
```

### 1.2. Hello, World!

Create a file named `hello.vn`:
```varian
print("Hello, World!")
```
Run the script using the Varian command line tool:
```bash
./vn run hello.vn
# Or use the shorthand:
./vn hello.vn
```

### 1.3. Tooling and Package Scaffolding

Varian includes built-in commands for package intent management and script wrapping:
*   **`vn add <pkg>`**: Records a package dependency in `varian.pkg`.
*   **`vn wrap python:<module>`**: Spawns a background Python subprocess to introspect public function signatures and generates a ready-to-use Varian proxy file under `vn_modules/<module>.vn`.

---

## 2. Programming a Guessing Game

Here is a simple interactive guessing game in Varian using standard library functions:
```varian
// Varian Guessing Game
let secret = math.random_int(1, 100)
print("I'm thinking of a number between 1 and 100.")

let attempts = 0
loop {
    print("Enter your guess: ")
    let guess_str = io.read_line()
    if guess_str == null {
        print("Goodbye!")
        break
    }
    
    let guess = string_to_int(guess_str.trim())
    attempts = attempts + 1
    
    if guess < secret {
        print("Too low!")
    } else if guess > secret {
        print("Too high!")
    } else {
        print("Correct! You got it in " + attempts + " attempts.")
        break
    }
}
```

---

## 3. Common Programming Concepts

### 3.1. Variables and Mutability

Variable bindings are declared using `let`. Reassignment does not use the `let` keyword:
```varian
let x = 5
x = x + 1
```
> [!NOTE]
> Type annotations are parsed and validated by static analysis tooling (`vn lint`), but they are discarded before runtime execution. The VM remains fully dynamically typed.
```varian
let price: float = 9.99 // Annotation discarded at execution time
```

### 3.2. Data Types

Varian supports the following primitive data types:
*   `bool`: `true` or `false`
*   `int`: Platform-native integer representation
*   `float`: IEEE-754 floating-point representation
*   `string`: Immutable, UTF-8 encoded sequence of bytes
*   `null`: Representation of empty/uninitialized value
*   Arrays: Copy-on-write dynamic slices (`[1, 2, 3]`)
*   Tuples: Stack-allocated fixed-size values (`(1, "str")`)
*   Structs & Enums

### 3.3. Functions

Declared using `fn`. Functions support optional parameter and return type signatures:
```varian
fn add(a: int, b: int) -> int {
    return a + b
}
```
Multiple return values are supported via comma separation:
```varian
fn divide(a, b) {
    if b == 0 {
        return null, "division by zero"
    }
    return a / b, null
}
```

### 3.4. Comments

Line comments begin with `//`:
```varian
// This is a line comment.
```

### 3.5. Control Flow

Varian supports conventional blocks and exclusive ranges:
```varian
if x > 10 {
    print("Big")
} else {
    print("Small")
}

for i in 0..10 {
    print(i) // Outputs 0 through 9
}

while x < 100 {
    x = x + 10
}
```

---

## 4. Understanding Memory & Mutation

### 4.1. Struct Mutation (In-Place)

Structs are allocated on the heap (or the task-local bump arena) and are passed by reference. Method calls and assignments mutate the underlying object directly:
```varian
struct Point { x: int, y: int }
let p = Point { x: 0, y: 0 }
p.x = 10 // Mutates the object p refers to in-place
```

### 4.2. Array Mutation (Copy-On-Write)

Unlike structs, Varian arrays utilize **copy-on-write semantics** via their methods (e.g., `.push()`). Appending elements returns a new array and leaves the original untouched:
```varian
let a = [1, 2]
let b = a.push(3)
print(a.len()) // 2
print(b.len()) // 3
```
> [!TIP]
> To append to an array field within a struct, reassign the field directly:
```varian
self.items = self.items.push(new_item)
```

### 4.3. The Garbage Collector

A Mark-and-Sweep garbage collector tracks all heap objects (`vm->objects`). However, to avoid allocation overhead during critical paths (e.g., HTTP request loops), Varian includes a per-task **bump arena** allocator (`task_arena_alloc`). Arena-backed structs are freed in bulk when a task is recycled.

---

## 5. Structs and Methods

### 5.1. Defining and Instantiating Structs

Structs group related data:
```varian
struct User {
    id: int,
    name: string,
    email: string,
}

let user = User { id: 42, name: "Alice", email: "alice@example.com" }
```

### 5.2. Struct Field Decorators & Validation

Varian supports built-in decorators on struct fields. These validation rules execute immediately during construction:
```varian
struct SignupForm {
    @is_email email: string,
    @min_len(3) name: string,
}
```
Available validations include: `is_email`, `is_url`, `is_alphanumeric`, `min_len(n)`, `max_len(n)`, and `is_uuid`.

### 5.3. Methods and `impl` Blocks

Behavior is attached to structs using `impl` blocks, with methods taking an explicit `self` reference:
```varian
impl User {
    fn display(self) {
        print(self.name + " <" + self.email + ">")
    }
}
```

---

## 6. Enums and Pattern Matching

### 6.1. Defining an Enum

Enums represent data that can be one of several variants, optionally holding associated payload data (similar to Rust):
```varian
enum WebResult<T> {
    Success(T),
    Failure(string),
}
```

### 6.2. Match Control Flow Construct

Use the `match` block to inspect enums and unpack their payloads:
```varian
let res = WebResult::Success(200)

match res {
    WebResult::Success(status) => print("Success: " + status),
    WebResult::Failure(err) => print("Failed: " + err)
}
```

---

## 7. Packages, Modules, and the Prelude

### 7.1. Package Structure with `varian.pkg`

Varian projects track metadata and dependencies using `varian.pkg`:
```toml
name = "web-service"
version = "1.0.0"

[deps]
zenith = "latest"
```

### 7.2. Module Loading and the Concatenation Prelude

> [!IMPORTANT]
> Varian does not contain an `import` keyword in its syntax. 
Instead, the command-line compiler/runner automatically discovers the `vn_modules/` directory in the current working directory, concatenates all `.vn` files inside it together as a prelude, and compiles them in-scope at the top of the target file.

---

## 8. Common Collections

### 8.1. Storing Sequential Data with Arrays

Arrays are dynamic, type-erased lists of elements:
```varian
let list = [1, 2, "three", true]
```
Support methods include `.len()`, `.push(item)`, and index access `list[0]`.

### 8.2. Tuples

Tuples are fixed-length sequences of varying types:
```varian
let pair = (10, "Hello")
let val = pair.0
```

---

## 9. Error Handling

### 9.1. Null-Propagation with the `?` Operator

The `?` operator evaluates an expression. If it is `null`, the operator immediately short-circuits and returns `null` from the current function:
```varian
fn safe_div(a, b) {
    if b == 0 { return null }
    return a / b
}

fn try_operation(a, b) {
    let val = safe_div(a, b)? // Returns null from try_operation if division fails
    return val + 10
}
```

### 9.2. Recoverable Errors with `try/catch/throw`

For standard error recovery, Varian provides `try/catch` blocks:
```varian
try {
    throw("something went wrong")
} catch e {
    print("Caught error: " + e)
}
```

---

## 10. Generic Types, Traits, and Lifetimes

### 10.1. Type-Erased Generics

Generics are supported syntactically for static type checks and IDE diagnostics:
```varian
struct Holder<T> { val: T }
```
Generics are fully type-erased at compile-time and have no specialized representation or runtime speed penalties in the VM.

### 10.2. Implicit Structural Traits

Varian implements implicit structural typing. If a struct exposes the methods defined on a trait, it implicitly implements that trait without declaration:
```varian
trait Greeter {
    fn greet(self)
}

struct Robot {}
impl Robot {
    fn greet(self) { print("Beep Boop") }
}

fn do_greet(g: Greeter) {
    g.greet()
}
// Robot is accepted by do_greet automatically
```

### 10.3. Lifetimes

Lifetimes are not enforced by the compiler. Object lifetimes are managed dynamically by the runtime's garbage collector.

---

## 11. Writing Automated Tests

### 11.1. How to Write Tests

Run `vn test` to walk files inside a `tests/` directory. Tests are declared via `test` blocks:
```varian
test "math square root" {
    assert_eq(math.sqrt(16.0), 4.0)
}
```
Available assertion globals include: `assert_eq(a, b)`, `assert_ne(a, b)`, and `assert_throws(fn)`.

### 11.2. Mocking and Intercepting Native Modules

Tests can intercept and mock methods belonging to native modules:
```varian
let original = mock.intercept("sqlite", "query", |conn, sql, params| {
    return [Row { id: 1, name: "Mock User" }]
})

// Run tests using mock behavior...

mock.restore("sqlite", "query", original)
```

---

## 12. Building a Command Line Program

Varian CLI inputs can be parsed from builtins or read from files using the [io](file:///root/dev/VarianLang/docs/STDLIB.md#L42) module:
```varian
// CLI File reader
let filepath = "test.txt"
let contents = io.read_text(filepath)
if contents == null {
    print("File not found or empty.")
} else {
    print(contents)
}
```

---

## 13. Functional Features: Closures and Lambdas

Closures are created via pipe syntax `|args| { body }`. They **capture scope variables by value**:
```varian
let factor = 2
let double = |x| { return x * factor }
print(double(10)) // 20
```

---

## 14. Package Scaffolding & Foreign Wrappers

Import Python libraries natively without typing out boilerplate:
```bash
vn wrap python:numpy
```
This writes a Varian-native proxy module `vn_modules/numpy.vn` wrapping python execution:
```varian
let res = numpy.dot(data, weights)
```

---

## 15. Garbage Collection & Object Lifecycle

Heap structures are tracked in a unified linked list inside the VM. To safeguard objects that outlive request scopes, the runtime employs an escape analysis write barrier ([escape_promote](file:///root/dev/VarianLang/src/vm.c#L478)) that deep-copies arena-allocated structs to the GC heap.

---

## 16. Cooperative Concurrency

### 16.1. Spawning Tasks

Varian concurrent tasks are cooperative green threads scheduled in a single-threaded round-robin loop:
```varian
fn run_worker(id) {
    print("Task " + id + " running")
    task.yield()
    print("Task " + id + " completed")
}

task.spawn(run_worker, [1])
task.spawn(run_worker, [2])
```

### 16.2. Channels with Automated Backpressure

Channels allow message passing between tasks:
```varian
let ch = task.channel(2) // Buffered capacity of 2
```
*   Sending (`ch <- value`): Blocks the task if the channel buffer is full.
*   Receiving (`<- ch`): Blocks the task if the channel buffer is empty. Returns `null` if closed.

### 16.3. The Actor Model

Actors are isolated, concurrent state objects:
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
let total = c.get()
```

---

## 17. Asynchronous Programming Fundamentals

### 17.1. Cooperative Green-Thread Scheduling

Tasks must voluntarily yield control back to the scheduler via `task.yield()` or blocking channel/actor communications to avoid starvation in the thread loop.

### 17.2. Worker Pools and Background Jobs

Varian's worker libraries support background task pooling:
```varian
let pool = WorkerPool { ch: task.channel(100), count: 0, workers: 0 }
pool.spawn(4) // Spawns 4 tasks to execute concurrent jobs
```

---

## 18. Object-Oriented Patterns in Varian

State and behavior are decoupled. Interfaces are structural, supporting dependency injection patterns and clean structural abstractions.

---

## 19. Pattern Matching Syntax and Guards

Patterns in `match` blocks support value bindings, ranges, wildcards, and guard clauses:
```varian
match status_code {
    200 => print("Success"),
    400..499 => print("Client Error"),
    500 if server_active == false => print("Fatal Offline Error"),
    _ => print("Unknown Status")
}
```

---

## 20. Advanced Features

### 20.1. Direct C FFI via `libffi`

Bind shared library symbols directly:
```varian
@ffi("libm.so.6", "sqrt")
fn c_sqrt(x: c_double) -> c_double
```

### 20.2. The Embedded Python Bridge

Call Python libraries on the fly using `python.run`:
```varian
let result = python.run("json", "dumps", [[10, 20]])
```

### 20.3. Compile-Time Evaluation with `comptime`

Bake complex computations directly into the bytecode during compilation:
```varian
let cached_value = comptime {
    return 100 * 200 + 35
}
```

---

## 21. Final Project: Web Programming with Zenith

### 21.1. Building a Web Application

Zenith is a fast web framework built directly in Varian:
```varian
let app = new_app()
app.get("/welcome", |req| {
    return Response { status: 200, body: "Hello from Zenith!", content_type: "text/plain", _keep_open: false }
}, "Welcome Route")
app.listen(8080)
```

### 21.2. Middleware Chains

Middleware intercepts and executes logic surrounding requests:
```varian
app.add_middleware(|req, next| {
    print("Request: " + req.path)
    return next(req)
})
```

### 21.3. Comptime Database ORM

Write compiled type-safe database queries evaluated once at compile-time:
```varian
let select_user = comptime {
    select("users")
        .fields(["id", "name"])
        .where("id", "=")
        .limit(1)
        .build()
}

let bound = bind(select_user, [42])
let rows = run_sqlite(bound, conn)
```

---

## 22. Appendices

### 22.1. Appendix A: Keywords and Special Identifiers

*   `let`, `fn`, `struct`, `impl`, `actor`, `enum`, `trait`, `match`, `try`, `catch`, `throw`, `loop`, `break`, `continue`, `while`, `for`, `in`, `null`.

### 22.2. Appendix B: Language Tooling (`vn fmt` and `vn lint`)

*   **`vn fmt`**: Automatically formats a Varian file.
*   **`vn lint`**: Runs static analysis checks (correctness, security, and performance violations).
