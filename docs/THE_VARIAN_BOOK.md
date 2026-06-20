# The Varian Programming Language Book

This book is a comprehensive, chapter-by-chapter guide to the **Varian Programming Language**, a modern, fast, agentic programming language designed to bridge native performance with universal foreign function interoperability, cooperative actor-based concurrency, and high-productivity web programming.

---

## Table of Contents

*   [Foreword](#foreword)
*   [Introduction](#introduction)
*   [1. Getting Started](#1-getting-started)
    *   [1.1. Installation](#11-installation)
    *   [1.2. Hello, World!](#12-hello-world)
    *   [1.3. Hello, Cargo! (Equivalent: Varian Packages and Tooling)](#13-hello-cargo-equivalent-varian-packages-and-tooling)
*   [2. Programming a Guessing Game](#2-programming-a-guessing-game)
    *   [Setting Up a New Project](#setting-up-a-new-project)
    *   [Processing a Guess](#processing-a-guess)
    *   [Storing Values with Variables](#storing-values-with-variables)
    *   [Receiving User Input](#receiving-user-input)
    *   [Handling Potential Failure with null / catch](#handling-potential-failure-with-null--catch)
    *   [Printing Values with print Placeholders](#printing-values-with-print-placeholders)
    *   [Testing the First Part](#testing-the-first-part)
    *   [Generating a Secret Number](#generating-a-secret-number)
    *   [Increasing Functionality with a Module](#increasing-functionality-with-a-module)
    *   [Comparing the Guess to the Secret Number](#comparing-the-guess-to-the-secret-number)
    *   [Allowing Multiple Guesses with Looping](#allowing-multiple-guesses-with-looping)
    *   [Quitting After a Correct Guess](#quitting-after-a-correct-guess)
    *   [Handling Invalid Input](#handling-invalid-input)
    *   [Summary](#summary)
*   [3. Common Programming Concepts](#3-common-programming-concepts)
    *   [3.1. Variables and Mutability](#31-variables-and-mutability)
    *   [3.2. Data Types](#32-data-types)
    *   [3.3. Functions](#33-functions)
    *   [3.4. Comments](#34-comments)
    *   [3.5. Control Flow](#35-control-flow)
*   [4. Understanding Memory, Mutation, and Lifetimes](#4-understanding-memory-mutation-and-lifetimes)
    *   [4.1. What is Memory Management? (Struct Reference Semantics vs Copy-On-Write Arrays)](#41-what-is-memory-management-struct-reference-semantics-vs-copy-on-write-arrays)
    *   [4.2. In-Place Mutation and the self Reference](#42-in-place-mutation-and-the-self-reference)
    *   [4.3. Slices and Indexing Semantics](#43-slices-and-indexing-semantics)
*   [5. Using Structs to Structure Related Data](#5-using-structs-to-structure-related-data)
    *   [5.1. Defining and Instantiating Structs](#51-defining-and-instantiating-structs)
    *   [5.2. An Example Program Using Structs (with Validation Decorators)](#52-an-example-program-using-structs-with-validation-decorators)
    *   [5.3. Methods and impl Blocks](#53-methods-and-impl-blocks)
*   [6. Enums and Pattern Matching](#6-enums-and-pattern-matching)
    *   [6.1. Defining an Enum](#61-defining-an-enum)
    *   [6.2. The match Control Flow Construct](#62-the-match-control-flow-construct)
    *   [6.3. Concise Control Flow with optional chaining and null coalescing](#63-concise-control-flow-with-optional-chaining-and-null-coalescing)
*   [7. Packages, Modules, and Scoping](#7-packages-modules-and-scoping)
    *   [7.1. Packages and the varian.pkg format](#71-packages-and-the-varianpkg-format)
    *   [7.2. Module Loading and the Concatenation Prelude](#72-module-loading-and-the-concatenation-prelude)
    *   [7.3. Scope Resolution](#73-scope-resolution)
    *   [7.4. Automatic Module Inclusion](#74-automatic-module-inclusion)
    *   [7.5. Separating Modules into Different Files](#75-separating-modules-into-different-files)
*   [8. Common Collections](#8-common-collections)
    *   [8.1. Storing Lists of Values with Arrays](#81-storing-lists-of-values-with-arrays)
    *   [8.2. Storing UTF-8 Encoded Text with Strings](#82-storing-utf-8-encoded-text-with-strings)
    *   [8.3. Storing Keys with Associated Values in Hash Maps (Dynamic Structs)](#83-storing-keys-with-associated-values-in-hash-maps-dynamic-structs)
*   [9. Error Handling](#9-error-handling)
    *   [9.1. Unrecoverable Errors with throw](#91-unrecoverable-errors-with-throw)
    *   [9.2. Recoverable Errors with try/catch and ?](#92-recoverable-errors-with-trycatch-and-)
    *   [9.3. Optional return propagation vs throw](#93-optional-return-propagation-vs-throw)
*   [10. Generic Types, Traits, and Lifetimes](#10-generic-types-traits-and-lifetimes)
    *   [10.1. Type-Erased Generic Syntax](#101-type-erased-generic-syntax)
    *   [10.2. Implicit Structural Traits](#102-implicit-structural-traits)
    *   [10.3. Memory Lifetimes: GC vs Bump Arenas](#103-memory-lifetimes-gc-vs-bump-arenas)
*   [11. Writing Automated Tests](#11-writing-automated-tests)
    *   [11.1. How to Write Tests](#111-how-to-write-tests)
    *   [11.2. Controlling How Tests Are Run](#112-controlling-how-tests-are-run)
    *   [11.3. Mocking and Intercepting Native Modules](#113-mocking-and-intercepting-native-modules)
*   [12. Building a Command Line Program](#12-building-a-command-line-program)
    *   [12.1. Accepting Command Line Arguments](#121-accepting-command-line-arguments)
    *   [12.2. Reading a File](#122-reading-a-file)
    *   [12.3. Improving Modularity and Error Isolation](#123-improving-modularity-and-error-isolation)
    *   [12.4. Test-Driven CLI Refactoring](#124-test-driven-cli-refactoring)
    *   [12.5. Working with Environment Variables](#125-working-with-environment-variables)
    *   [12.6. Redirecting Errors to Standard Error](#126-redirecting-errors-to-standard-error)
*   [13. Functional Features: Iterators and Closures](#13-functional-features-iterators-and-closures)
    *   [13.1. Closures and Value Capture](#131-closures-and-value-capture)
    *   [13.2. Processing Sequential Items](#132-processing-sequential-items)
    *   [13.3. Applying Iterators to the CLI Project](#133-applying-iterators-to-the-cli-project)
    *   [13.4. Performance in Loops vs. Map Functions](#134-performance-in-loops-vs-map-functions)
*   [14. Varian Tooling and Ecosystem](#14-varian-tooling-and-ecosystem)
    *   [14.1. Customizing Runtime Executable Flags](#141-customizing-runtime-executable-flags)
    *   [14.2. Local Project Scaffolding and Publishing Scenarios](#142-local-project-scaffolding-and-publishing-scenarios)
    *   [14.3. Multi-Module Project Workspaces](#143-multi-module-project-workspaces)
    *   [14.4. Installing the vn CLI Globally](#144-installing-the-vn-cli-globally)
    *   [14.5. Extending vn via Custom Wrappers](#145-extending-vn-via-custom-wrappers)
*   [15. Allocation, Smart Pointers, and GC Internals](#15-allocation-smart-pointers-and-gc-internals)
    *   [15.1. Heap Allocation and Sweep Cycles](#151-heap-allocation-and-sweep-cycles)
    *   [15.2. Treating Heap Objects as References](#152-treating-heap-objects-as-references)
    *   [15.3. GC Sweep Reclaim and Garbage Collection Details](#153-gc-sweep-reclaim-and-garbage-collection-details)
    *   [15.4. VM Object Reference Counts](#154-vm-object-reference-counts)
    *   [15.5. Task-Local Bump Arenas and the escape_promote Write Barrier](#155-task-local-bump-arenas-and-the-escape_promote-write-barrier)
    *   [15.6. Reference Cycles and Cycle Collection](#156-reference-cycles-and-cycle-collection)
*   [16. Cooperative Concurrency](#16-cooperative-concurrency)
    *   [16.1. Green-Thread Tasks](#161-green-thread-tasks)
    *   [16.2. Channels and Automated Backpressure](#162-channels-and-automated-backpressure)
    *   [16.3. Stateful Actors and Isolation (Preventing Shared-State Hazards)](#163-stateful-actors-and-isolation-preventing-shared-state-hazards)
    *   [16.4. Task Yielding Scheduling Safety](#164-task-yielding-scheduling-safety)
*   [17. Fundamentals of Asynchronous Programming](#17-fundamentals-of-asynchronous-programming)
    *   [17.1. Cooperative Scheduling Loop](#171-cooperative-scheduling-loop)
    *   [17.2. Applying Concurrency with Async Tasks](#172-applying-concurrency-with-async-tasks)
    *   [17.3. Working with Multiple Channels (Select Multiplexing)](#173-working-with-multiple-channels-select-multiplexing)
    *   [17.4. Continuous Streams and Channels](#174-continuous-streams-and-channels)
    *   [17.5. Structural Interfaces for Async](#175-structural-interfaces-for-async)
    *   [17.6. Futures, Tasks, and Threads (Single-Threaded Worker Pools)](#176-futures-tasks-and-threads-single-threaded-worker-pools)
*   [18. Object-Oriented Patterns in Varian](#18-object-oriented-patterns-in-varian)
    *   [18.1. Characteristics of Object-Oriented Languages (Encapsulation)](#181-characteristics-of-object-oriented-languages-encapsulation)
    *   [18.2. Trait Objects and Structural Typing Parameters](#182-trait-objects-and-structural-typing-parameters)
    *   [18.3. Decoupling State and Behavior with impl Blocks](#183-decoupling-state-and-behavior-with-impl-blocks)
*   [19. Pattern Matching and Guards](#19-pattern-matching-and-guards)
    *   [19.1. Where Pattern Matching Applies](#191-where-pattern-matching-applies)
    *   [19.2. Match Guards and Exhaustiveness Warnings](#192-match-guards-and-exhaustiveness-warnings)
    *   [19.3. Pattern Syntax (Constants, Ranges, Wildcards, Bindings)](#193-pattern-syntax-constants-ranges-wildcards-bindings)
*   [20. Advanced Features](#20-advanced-features)
    *   [20.1. Direct C FFI via @ffi](#201-direct-c-ffi-via-ffi)
    *   [20.2. Advanced Structural Traits](#202-advanced-structural-traits)
    *   [20.3. Runtime Primitive Casting and Type Validation](#203-runtime-primitive-casting-and-type-validation)
    *   [20.4. First-Class Functions and Closures](#204-first-class-functions-and-closures)
    *   [20.5. Compile-Time Evaluation with comptime](#205-compile-time-evaluation-with-comptime)
*   [21. Final Project: Web Programming with Zenith](#21-final-project-web-programming-with-zenith)
    *   [21.1. Zenith App Routing and Tri-Tries](#211-zenith-app-routing-and-tri-tries)
    *   [21.2. Middleware Chains](#212-middleware-chains)
    *   [21.3. Comptime Database ORM](#213-comptime-database-orm)
    *   [21.4. Graceful Shutdown and Cleanup](#214-graceful-shutdown-and-cleanup)
*   [22. Appendices](#22-appendices)
    *   [22.1. Appendix A: Keywords and Special Identifiers](#221-appendix-a-keywords-and-special-identifiers)
    *   [22.2. Appendix B: Language Tooling (vn fmt and vn lint)](#222-appendix-b-language-tooling-vn-fmt-and-vn-lint)
    *   [22.3. Appendix C: Field Decorators](#223-appendix-c-field-decorators)
    *   [22.4. Appendix D: Useful Development Tools](#224-appendix-d-useful-development-tools)
    *   [22.5. Appendix E: Editions and VM Releases](#225-appendix-e-editions-and-vm-releases)
    *   [22.6. Appendix F: Global Documentation and Translations](#226-appendix-f-global-documentation-and-translations)
    *   [22.7. Appendix G: The owo Syntax Color Scheme](#227-appendix-g-the-owo-syntax-color-scheme)

---

## Foreword

Varian is built for high-performance network services, AI-agent integrations, and systems programming. It represents a synthesis of three core paradigms:
1.  **Direct Native Control**: Direct shared-library bindings via libffi without wrapper code.
2.  **Universal Foreign Bridges**: Embedded Python runtime bridge to invoke any library.
3.  **Cooperative Green Threads**: Tasks and actor communication built on a lightweight, single-threaded VM scheduler.

---

## Introduction

Varian is dynamically typed but incorporates optional static type annotations. The code compiles to custom bytecode, which executes on an optimized VM using task-local struct arenas and label-as-values computed-goto dispatches for throughput.

---

## 1. Getting Started

### 1.1. Installation

Build the runtime executable `vn` from source:
```bash
make clean
make -j8 CFLAGS="-Wall -Wextra -std=gnu11 -O2 -DNDEBUG -Iinclude"
```
Launch the interactive REPL:
```bash
./vn
```

### 1.2. Hello, World!

Create a file named `hello.vn`:
```varian
print("Hello, World!")
```
Run it via the CLI:
```bash
./vn hello.vn
```

### 1.3. Hello, Cargo! (Equivalent: Varian Packages and Tooling)

Dependencies are tracked in `varian.pkg`. Varian manages local packages in the `vn_modules/` folder.
To add a package, write:
```bash
vn add zenith
```
This updates the package manifest automatically.

---

## 2. Programming a Guessing Game

In this chapter, we will build an interactive guessing game to demonstrate Varian's core control flow, variable bindings, and standard input/output routines.

### Setting Up a New Project

Create a directory for your project and place any needed helper modules inside the `vn_modules/` folder.

### Processing a Guess

We begin by reading from standard input. Create `main.vn`:
```varian
print("Guess the number!")
```

### Storing Values with Variables

We store values using `let`:
```varian
let guess = ""
```

### Receiving User Input

Read lines from input using `io.read_line()`:
```varian
print("Please input your guess.")
let guess_str = io.read_line()
```

### Handling Potential Failure with null / catch

If the input stream ends, `io.read_line()` returns `null`. We handle this using conditional checks:
```varian
if guess_str == null {
    print("No input detected. Exiting.")
}
```

### Printing Values with print Placeholders

Varian strings can be concatenated with `+`. Print variables directly:
```varian
print("You guessed: " + guess_str)
```

### Testing the First Part

Run `./vn main.vn` to test that input is successfully accepted and echoed back.

### Generating a Secret Number

Use the native `math` module to generate a random number between 1 and 100:
```varian
let secret = math.random_int(1, 100)
```

### Increasing Functionality with a Module

If we need more complex logic, we can write helper methods inside a `.vn` file in `vn_modules/`.

### Comparing the Guess to the Secret Number

We parse the input string into an integer and compare it:
```varian
let guess = string_to_int(guess_str.trim())
if guess < secret {
    print("Too low!")
} else if guess > secret {
    print("Too high!")
} else {
    print("You win!")
}
```

### Allowing Multiple Guesses with Looping

Wrap the guess processing in a `loop` block:
```varian
loop {
    let guess_str = io.read_line()
    if guess_str == null { break }
    // comparisons here...
}
```

### Quitting After a Correct Guess

Use the `break` statement to exit the loop once the correct guess is entered.

### Handling Invalid Input

To prevent crash failures when parsing bad strings, wrap the conversion inside a `try/catch` or check for `null`:
```varian
try {
    let guess = string_to_int(guess_str.trim())
    // process guess...
} catch e {
    print("Please type a valid number!")
}
```

### Summary

You have built a fully interactive game demonstrating Varian variables, loops, input processing, and parsing.

---

## 3. Common Programming Concepts

### 3.1. Variables and Mutability

Variable bindings are declared with `let`. They can be reassigned freely:
```varian
let x = 5
x = 6
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

## 4. Understanding Memory, Mutation, and Lifetimes

### 4.1. What is Memory Management? (Struct Reference Semantics vs Copy-On-Write Arrays)

Instead of Rust's ownership system, Varian employs a hybrid approach:
*   **Structs** are passed by reference and managed by a garbage collector.
*   **Arrays** utilize copy-on-write (COW) semantics when mutated via methods (like `.push()`).

### 4.2. In-Place Mutation and the self Reference

Inside struct `impl` blocks, methods take an explicit `self` reference. Mutating fields on `self` edits the struct in place:
```varian
struct Point { x: int, y: int }
impl Point {
    fn move_to(self, new_x, new_y) {
        self.x = new_x
        self.y = new_y
    }
}
```

### 4.3. Slices and Indexing Semantics

Arrays and strings can be sliced and indexed. While array indexing is mutable in-place (`arr[0] = x`), methods that append or resize copy the underlying array buffer.

---

## 5. Using Structs to Structure Related Data

### 5.1. Defining and Instantiating Structs

Structs group related fields:
```varian
struct User {
    id: int,
    name: string,
    email: string,
}

let user = User { id: 42, name: "Alice", email: "alice@example.com" }
```

### 5.2. An Example Program Using Structs (with Validation Decorators)

Validation decorators on struct fields enforce conditions at construction time:
```varian
struct SignupForm {
    @is_email email: string,
    @min_len(3) name: string,
}
```

### 5.3. Methods and impl Blocks

Behavior is attached to structs using `impl` blocks:
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

Enums represent sum types that can contain payload data:
```varian
enum WebResult<T> {
    Success(T),
    Failure(string),
}
```

### 6.2. The match Control Flow Construct

Use the `match` block to inspect enums and unpack payload bindings:
```varian
let res = WebResult::Success(200)

match res {
    WebResult::Success(status) => print("Success: " + status),
    WebResult::Failure(err) => print("Failed: " + err)
}
```

### 6.3. Concise Control Flow with optional chaining and null coalescing

Null-coalescing (`??`) and optional chaining (`?.`) permit concise, nil-safe expressions:
```varian
let role = user?.profile?.role ?? "guest"
```

---

## 7. Packages, Modules, and Scoping

### 7.1. Packages and the varian.pkg format

Packages declare metadata and target dependencies:
```toml
name = "web-service"
version = "1.0.0"

[deps]
zenith = "latest"
```

### 7.2. Module Loading and the Concatenation Prelude

Varian has no `import` keyword in its syntax. The CLI runner automatically concatenates all `.vn` files inside the `vn_modules/` folder as a prelude compiled in-scope above your entry file.

### 7.3. Scope Resolution

Names inside the prelude are loaded at global scope, while closures and inner scopes resolve locally.

### 7.4. Automatic Module Inclusion

By placing libraries under `vn_modules/`, they are automatically imported into scope across all project script files.

### 7.5. Separating Modules into Different Files

Larger applications separate concerns by writing distinct `.vn` files inside the `vn_modules/` directory.

---

## 8. Common Collections

### 8.1. Storing Lists of Values with Arrays

Arrays are dynamic, type-erased lists of elements:
```varian
let list = [1, 2, "three", true]
```
Adding to an array returns a new copy:
```varian
let updated = list.push("four")
```

### 8.2. Storing UTF-8 Encoded Text with Strings

Strings are immutable sequences of UTF-8 text. String manipulation methods return new string values:
```varian
let text = "Hello"
let upper = text.upper()
```

### 8.3. Storing Keys with Associated Values in Hash Maps (Dynamic Structs)

Varian uses structs to map keys to values, or builds them dynamically via `http.create_struct(keys, values)`.

---

## 9. Error Handling

### 9.1. Unrecoverable Errors with throw

Throwing directly aborts execution if not handled in a try/catch:
```varian
throw("unrecoverable error")
```

### 9.2. Recoverable Errors with try/catch and ?

For standard error recovery, Varian provides `try/catch` blocks:
```varian
try {
    throw("something went wrong")
} catch e {
    print("Caught error: " + e)
}
```
The `?` operator evaluates an expression. If it is `null`, it immediately propagates `null` as the return value of the enclosing function:
```varian
fn try_operation(a, b) {
    let val = safe_div(a, b)? // returns null early if division fails
    return val + 10
}
```

### 9.3. Optional return propagation vs throw

Throwing errors is reserved for unexpected failures, while `null` and `?` are preferred for expected failures like missing keys.

---

## 10. Generic Types, Traits, and Lifetimes

### 10.1. Type-Erased Generic Syntax

Generics are supported syntactically for static type checks and IDE diagnostics:
```varian
struct Holder<T> { val: T }
```
They are fully type-erased at compile-time and have no runtime speed penalties.

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

### 10.3. Memory Lifetimes: GC vs Bump Arenas

Unlike Rust, lifetimes are handled dynamically by the VM's garbage collector or reclaimed in bulk within per-task bump arenas.

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

### 11.2. Controlling How Tests Are Run

Run specific files or pass test paths to `vn test <path>` to narrow scope.

### 11.3. Mocking and Intercepting Native Modules

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

### 12.1. Accepting Command Line Arguments

Arguments are passed directly to script execution and are readable via system bindings.

### 12.2. Reading a File

Varian CLI inputs can be read from files using the `io` module:
```varian
let contents = io.read_text("input.txt")
```

### 12.3. Improving Modularity and Error Isolation

Break code into functional components and catch file access errors gracefully using `try/catch`.

### 12.4. Test-Driven CLI Refactoring

Write test blocks in the test folder to assert that CLI outputs are correctly processed.

### 12.5. Working with Environment Variables

Varian queries the system environment using built-in config wrappers or via Python bridge.

### 12.6. Redirecting Errors to Standard Error

Errors in standard routines can be targeted specifically to standard error flows.

---

## 13. Functional Features: Iterators and Closures

### 13.1. Closures and Value Capture

Closures are created via pipe syntax `|args| { body }`. They capture scope variables **by value**:
```varian
let factor = 2
let double = |x| { return x * factor }
print(double(10)) // 20
```

### 13.2. Processing Sequential Items

Loop constructs and range expressions iterate through arrays efficiently.

### 13.3. Applying Iterators to the CLI Project

Process file lines sequentially inside iterator-like ranges:
```varian
let lines = contents.split("\n")
for i in 0..lines.len() {
    print("Line " + i + ": " + lines[i])
}
```

### 13.4. Performance in Loops vs. Map Functions

Simple loop ranges execute at native VM speed via computed-goto, bypassing overhead from closure call allocations.

---

## 14. Varian Tooling and Ecosystem

### 14.1. Customizing Runtime Executable Flags

Set environment variables to debug or optimize performance:
*   `VN_DEBUG_AST=1`: Outputs AST.
*   `VN_DEBUG_BYTECODE=1`: Outputs bytecode disassembly.

### 14.2. Local Project Scaffolding and Publishing Scenarios

Scaffold new projects with `vn add` to declare local package dependencies.

### 14.3. Multi-Module Project Workspaces

Track dependencies across nested workspaces using the `varian.pkg` format.

### 14.4. Installing the vn CLI Globally

Compile and copy the `vn` executable to system paths (e.g. `/usr/local/bin`).

### 14.5. Extending vn via Custom Wrappers

Auto-generate Varian modules for foreign code:
```bash
vn wrap python:numpy
```

---

## 15. Allocation, Smart Pointers, and GC Internals

### 15.1. Heap Allocation and Sweep Cycles

Varian manages dynamic memory on the heap. Allocation triggers are periodically swept by a Mark-and-Sweep GC.

### 15.2. Treating Heap Objects as References

Passing a struct into functions shares its memory address. Changes made within functions mutate the caller's struct value in-place.

### 15.3. GC Sweep Reclaim and Garbage Collection Details

Object reference counters identify roots. Unreferenced items are recycled to prevent leaks.

### 15.4. VM Object Reference Counts

Every heap object tracking header contains a reference counter to assist GC reachability determinations.

### 15.5. Task-Local Bump Arenas and the escape_promote Write Barrier

To achieve high throughput (e.g. in HTTP request routing), Varian spawns request tasks with per-task bump arenas. If an arena object escapes to a channel, actor, or global binding, the `escape_promote` write barrier copies it to the GC heap.

### 15.6. Reference Cycles and Cycle Collection

The Mark-and-Sweep garbage collector resolves circular references that cannot be collected by pure reference counting.

---

## 16. Cooperative Concurrency

### 16.1. Green-Thread Tasks

Varian runs cooperative green threads scheduled in a single-threaded round-robin loop:
```varian
fn run_worker(id) {
    print("Task " + id + " running")
    task.yield()
    print("Task " + id + " completed")
}

task.spawn(run_worker, [1])
```

### 16.2. Channels and Automated Backpressure

Channels enable safe data transfer between tasks:
```varian
let ch = task.channel(2) // buffer capacity of 2
```
*   `ch <- value` yields the sending task if the buffer is full.
*   `<- ch` yields the receiving task if the buffer is empty.

### 16.3. Stateful Actors and Isolation (Preventing Shared-State Hazards)

Actors isolate state. Calling an actor method yields the caller and schedules message execution within the actor's thread loop:
```varian
actor Counter {
    val: int = 0,
    fn increment(self) {
        self.val = self.val + 1
    }
}
let c = Counter.spawn()
c.increment()
```

### 16.4. Task Yielding Scheduling Safety

Because Varian schedules cooperatively, tasks must call `task.yield()` or perform I/O to share scheduler threads.

---

## 17. Fundamentals of Asynchronous Programming

### 17.1. Cooperative Scheduling Loop

The core scheduler loops over active tasks, yielding execution contexts cleanly.

### 17.2. Applying Concurrency with Async Tasks

Spawn concurrent async functions and interact with channels to manage background data flow.

### 17.3. Working with Multiple Channels (Select Multiplexing)

Read from channels sequentially or implement polling constructs to prevent task blockages.

### 17.4. Continuous Streams and Channels

Read stream chunks off persistent channels continuously to process data fragments.

### 17.5. Structural Interfaces for Async

Tasks utilize implicit structural typing to resolve stream and channel types.

### 17.6. Futures, Tasks, and Threads (Single-Threaded Worker Pools)

Background workers utilize channels to process task jobs asynchronously:
```varian
let pool = WorkerPool { ch: task.channel(100), count: 0, workers: 0 }
pool.spawn(4)
```

---

## 18. Object-Oriented Patterns in Varian

### 18.1. Characteristics of Object-Oriented Languages (Encapsulation)

Varian encapsulates data in structs and behavior in `impl` blocks.

### 18.2. Trait Objects and Structural Typing Parameters

Implicit traits function as trait objects, enabling polymorphic method dispatching.

### 18.3. Decoupling State and Behavior with impl Blocks

Keep structs clean by isolating their algorithms inside decoupled implementation blocks.

---

## 19. Pattern Matching and Guards

### 19.1. Where Pattern Matching Applies

Pattern matching is supported inside `match` blocks for structs, enums, values, and range checks.

### 19.2. Match Guards and Exhaustiveness Warnings

Apply `if` conditions as guards on match arms:
```varian
match status_code {
    500 if server_active == false => print("Fatal Error"),
    _ => print("Default case")
}
```

### 19.3. Pattern Syntax (Constants, Ranges, Wildcards, Bindings)

Varian matches integers, strings, enum constructors, and wildcards (`_`).

---

## 20. Advanced Features

### 20.1. Direct C FFI via @ffi

Bind C shared libraries directly without wrapper scripts:
```varian
@ffi("libm.so.6", "sqrt")
fn c_sqrt(x: c_double) -> c_double
```

### 20.2. Advanced Structural Traits

Traits are resolved during static linting to ensure method compliance across inputs.

### 20.3. Runtime Primitive Casting and Type Validation

Cast strings to numbers using native parsing libraries, or check type fields dynamically.

### 20.4. First-Class Functions and Closures

Pass functions and closures as variables, capturing environmental scope parameters by value.

### 20.5. Compile-Time Evaluation with comptime

Bake query values or math constants during compilation:
```varian
let val = comptime {
    return 100 * 200
}
```

---

## 21. Final Project: Web Programming with Zenith

### 21.1. Zenith App Routing and Tri-Tries

Zenith registers route paths using segment-based radix tries:
```varian
let app = new_app()
app.get("/welcome", |req| {
    return Response { status: 200, body: "Welcome!", content_type: "text/plain" }
}, "Welcome Route")
```

### 21.2. Middleware Chains

Middlewares intercept and process requests sequentially:
```varian
app.add_middleware(|req, next| {
    print("Path: " + req.path)
    return next(req)
})
```

### 21.3. Comptime Database ORM

Bake query statements at compile time to eliminate string-concatenation SQL overhead:
```varian
let select_user = comptime {
    select("users")
        .fields(["id", "name"])
        .where("id", "=")
        .build()
}
```

### 21.4. Graceful Shutdown and Cleanup

Close listening server sockets and finish active worker channel jobs prior to exiting.

---

## 22. Appendices

### 22.1. Appendix A: Keywords and Special Identifiers

`let`, `fn`, `struct`, `impl`, `actor`, `enum`, `trait`, `match`, `try`, `catch`, `throw`, `loop`, `break`, `continue`, `while`, `for`, `in`, `null`.

### 22.2. Appendix B: Language Tooling (vn fmt and vn lint)

*   `vn fmt`: Comment-preserving re-lexer.
*   `vn lint`: AST-walking lint rules (detects correctness, security, performance).

### 22.3. Appendix C: Field Decorators

Supported rules: `@is_email`, `@is_url`, `@is_alphanumeric`, `@min_len(n)`, `@max_len(n)`, `@is_uuid`.

### 22.4. Appendix D: Useful Development Tools

Use `vn doctor` to check DB states, env variables, N+1 loops, and project code hygiene.

### 22.5. Appendix E: Editions and VM Releases

Varian follows semantic version releases tracked under the language specification roadmap.

### 22.6. Appendix F: Global Documentation and Translations

Varian documentation supports internationalization standards to localize compiler warnings and specs.

### 22.7. Appendix G: The owo Syntax Color Scheme

Varian editor extensions utilize the **owo color scheme**, designed to present a high-contrast, playful syntax layout distinct from standard language themes:

| Element | Color Type | Hex Code | Description |
| :--- | :--- | :--- | :--- |
| **Keywords** | Pastel Pink | `#ff9ebb` | Used for `let`, `fn`, `struct`, `impl`, `match`, `loop`, etc. |
| **Operators** | Warm Coral | `#ff6f69` | Highlight for `<-`, `?.`, `??`, `+`, `-`, `=`, and comparative tokens. |
| **Functions** | Soft Lavender | `#d0bdf4` | Applied to function declarations and method call identifiers. |
| **Types** | Mint Green | `#a8e6cf` | Used for type annotations (`int`, `float`, `string`, `c_double`). |
| **Strings** | Creamy Peach | `#ffd3b6` | Represents string literals and character data. |
| **Numbers** | Butter Yellow | `#fffaaa` | Highlights constant integer and float values. |
| **Comments** | Slate Gray | `#8892b0` | Demarcates line comments and documentation text. |
| **Decorators** | Neon Purple | `#b388ff` | Identifies field constraints and macros like `@ffi` or `@is_email`. |
