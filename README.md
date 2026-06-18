# VarianLang

A systems programming language built from scratch in C with a custom bytecode VM.

## Phase 1: Core Compiler & Runtime Engine ✓

The raw execution pipeline is bulletproof. Transforms source code into executable bytecode through a hand-written recursive descent parser and custom stack-based VM.

### Features
- **Lexer & Parser:** Full recursive descent parser with string interpolation (`"hello {name}"`), regex literals, and operator precedence
- **Primitive Types:** `bool`, `int`, `float`, `string`, byte slices (`b"..."`)
- **Control Flow:** `if/else`, `while`, `for` (with range `..`), `loop`, `break`/`continue`
- **Functions:** Declarations, calls, arrow shorthand (`fn name(params) => expr`), **multiple return values**
- **Lambda Expressions:** `|params| expr` syntax
- **Pattern Matching:** `match expr { pattern => body, ... }`
- **String Interpolation:** Automatic int/float/bool conversion
- **Error Handling:** `?` operator for nil propagation, `try`/`catch` with `throw()`
- **Line Numbers:** RLE-encoded line info in bytecode, proper stack traces

### Data Structures
- **Structs:** `struct Name { field: Type, ... }` with field access (`obj.field`)
- **Enums:** `enum Name { Variant, Variant(Type), ... }` with `Enum::Variant(args)` syntax
- **Methods:** `impl Type { fn method(self, ...) { ... } }` with dot-call sugar (`obj.method()`)
- Arrays, tuples

### Execution
Custom stack-based bytecode VM with:
- 40+ opcodes
- RLE-encoded line numbers for error reporting
- Native function support (`print`, `throw`)
- Global/local variable scoping

## Building

```sh
make
./vn examples/test.vn
```

## Usage

```sh
./vn <file.vn>     # Run a file
./vn                # Interactive REPL
```

### Debug environment variables
- `VN_DEBUG_AST` — Print AST before compilation
- `VN_DEBUG_BYTECODE` — Print bytecode disassembly

## Examples

```sh
# Simple
./vn examples/hello.vn
./vn examples/fib.vn

# Structs & methods
./vn examples/structs.vn
./vn examples/methods.vn

# Enums & pattern matching
./vn examples/enums.vn

# Multiple return values
./vn examples/multi_return.vn

# String interpolation
./vn examples/interp.vn

# Error handling
./vn examples/errors.vn
```

## Project Structure

```
├── include/          # Header files
│   ├── varian.h     # Core types, arena allocator
│   ├── lexer.h       # Token types, lexer state
│   ├── parser.h      # Parser state, struct/enum registries
│   ├── ast.h         # AST node types, constructors
│   └── vm.h          # Opcodes, value types, VM state
├── src/              # Implementation
│   ├── arena.c       # Arena allocator
│   ├── lexer.c       # Tokenizer
│   ├── parser.c      # Recursive descent parser
│   ├── ast.c         # AST construction & debug printing
│   ├── vm.c          # Compiler + bytecode VM
│   └── main.c        # CLI & REPL
├── examples/         # Example programs
├── Makefile
└── PLAN.md           # Full roadmap
```

## Architecture

Source → Lexer → Parser → AST → Compiler → Bytecode → VM

All frontend allocations use a fast arena allocator. The bytecode VM is a register-style stack machine with heap-allocated objects (strings, arrays, structs, enums).
