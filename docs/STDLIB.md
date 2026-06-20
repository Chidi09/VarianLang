# Standard Library

All native modules are pre-registered globals — there is no `import` statement in the
language at all (the lexer has no such keyword). `math.sqrt(9.0)`, `string`/`array`
methods, `sqlite`, etc. are simply always available. The `vn_modules/*.vn` files (Zenith,
the ORM, queue helpers) are different: they're plain Varian source that the CLI
concatenates as a prelude in front of your file every time you run `vn run <file>` (or
`vn <file>`) — see `docs/TOOLING.md`.

## `math`

```varian
print(math.sin(0.0))
print(math.cos(0.0))
print(math.sqrt(9.0))
print(math.abs(-5))
print(math.floor(3.7))
print(math.ceil(3.2))
```

## `string` and `array` — called as methods, not module functions

These are dispatched by *value type*, so you call them as methods on a string/array
value rather than as `string.len(s)`:

```varian
let s = "Hello World"
print(s.len())
print(s.upper())
print(s.lower())
print(s.substring(0, 5))
print(s.trim())
print(s.split(" "))
print(s.starts_with("Hello"))
print(s.replace("World", "Varian"))

let a = [1, 2, 3]
print(a.len())
let b = a.push(4)   // returns a NEW array — does not mutate `a`, see docs/LANGUAGE.md
```

## `io`

```varian
let ok = io.write_text("/tmp/file.txt", "Hello from Varian!")
let content = io.read_text("/tmp/file.txt")
let missing = io.read_text("/tmp/nonexistent")   // nil, not an error
```

## `sqlite`

```varian
let conn = sqlite.connect("test.db")   // or ":memory:"
sqlite.query(conn, "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)")
sqlite.query(conn, "INSERT INTO users (name) VALUES (?)", ["Alice"])
let rows = sqlite.query(conn, "SELECT * FROM users ORDER BY id ASC")
for i in 0..rows.len() {
    print(rows[i].id, rows[i].name)
}
sqlite.close(conn)
```

`sqlite.query(conn, sql)` or `sqlite.query(conn, sql, params)` — params is an array,
positionally bound to `?` placeholders. Rows come back as an array of structs with one
field per column. Prefer the comptime ORM in `vn_modules/db.vn` (see `docs/ZENITH.md`)
over hand-written SQL strings where the query shape is static — `vn lint` flags
string-concatenated SQL.

## `postgres`

Same shape as `sqlite`: `postgres.connect(...)`, `postgres.query(conn, sql, params)`,
`postgres.close(conn)`.

## `redis`

```varian
let red = redis.connect("127.0.0.1", 6379)
redis.cmd(red, "SET", ["mykey", "hello redis"])
let val = redis.cmd(red, "GET", ["mykey"])
redis.cmd(red, "RPUSH", ["mylist", "item1"])
let items = redis.cmd(red, "LRANGE", ["mylist", 0, -1])
redis.close(red)
```

`redis.cmd(conn, command_name, args_array)` is a generic command dispatcher — there's
one native entry point for every Redis command, not a method per command.

## `http`

```varian
let body = http.get("https://example.com")
print(body.len())

http.serve(8080, |req| {
    return Response { status: 200, body: "hi", content_type: "text/plain" }
})
```

`http.get(url)` returns the response body as a string. `http.serve(port, handler)`
starts a blocking HTTP server, calling `handler(req)` per request — this is what
`ZenithApp.listen()` wraps (see `docs/ZENITH.md`). 

> [!TIP]
> Under the hood, `http.serve` configures each connection request handler task with a **per-task struct arena** via `task_arena_enable()`. This avoids GC and heap allocation overhead entirely for request/response structs, reclaiming them in bulk upon completion.

`http.create_struct(keys_array, values_array)` builds a struct dynamically from parallel arrays — used throughout Zenith to build request/params objects with field sets not known until runtime.

## `auth`

```varian
let hash = auth.hash_sha256("hello")

struct Payload { user_id: int, role: string }
let token = auth.sign_jwt(Payload { user_id: 42, role: "admin" }, "mysecret")
let decoded = auth.verify_jwt(token, "mysecret")
print(decoded.role)
```

## `validate`

These are the functions backing the `@is_email`/`@min_len(n)` etc. struct field
decorators (see `docs/LANGUAGE.md`) — `is_email`, `is_url`, `is_alphanumeric`,
`min_len(n)`, `max_len(n)`, `is_uuid`. They can also be called directly:
`validate.is_email(s)` returns a bool.

## `sanitize`

`sanitize.strip_html(s)`, `sanitize.escape_html(s)`, `sanitize.trim(s)`.

## `regex` — POSIX extended regular expressions

Backed by libc `regcomp`/`regexec` (POSIX ERE). There are **no** `\d`/`\w` shorthands —
use character classes (`[0-9]`, `[A-Za-z]`). Every function takes an optional trailing
`flags` string: `"i"` for case-insensitive, `"m"` for multi-line (`^`/`$` match at line
breaks).

```varian
regex.test("[0-9]+", "abc123")              // true
regex.test("hello", "HELLO", "i")           // true (case-insensitive)
regex.match("[0-9]+", "order 42 now")       // "42"  (first match, or null)
regex.find_all("[0-9]+", "a1 b22 c333")     // ["1", "22", "333"]
regex.groups("([a-z]+)@([a-z]+)", "ada@example")  // ["ada@example", "ada", "example"]
regex.replace("[0-9]+", "a1b2", "#")        // "a#b#"  (replaces all)
regex.replace("([a-z])([0-9])", "a1", "\\2\\1")   // "1a"  (\1..\9 backrefs, \0 = whole)
```

`match`/`groups` return `null` when there is no match; `find_all` returns `[]`.
Note `match` is a reserved keyword but works fine after `.` (see `docs/LANGUAGE.md`).

## `json_encode` / `json_decode`

Plain globals, not a module (no `json.` prefix):

```varian
let s = json_encode(some_struct_or_array_or_primitive)
let v = json_decode(s)
```

Works generically over structs, arrays, and primitives — this is what the OpenAPI spec
generation and several test helpers lean on rather than hand-building JSON strings.

## `python` — calling into Python

```varian
let result = python.run("json", "dumps", [[1, 2, 3]])
let nums = python.run("json", "loads", ["[1, 2, 3]"])
let s = python.run("math", "sqrt", [9])
```

`python.run(module_name, function_name, args_array)` imports `module_name` in an
embedded Python interpreter, calls `function_name(*args)`, and converts the result back
to a Varian value. This is the documented escape hatch for anything not worth writing a
native C module for (e.g. `boto3` for S3 — see `docs/ZENITH.md`'s note on
"Untouchables").

## FFI — calling C directly

```varian
@ffi("libm.so.6", "sqrt")
fn fast_sqrt(x: c_double) -> c_double

@ffi("/tmp/libffi_helper.so", "greet")
fn greet(name: ptr) -> ptr

print(fast_sqrt(9.0))
let raw = greet("World")          // ptr params decay to a raw address (printed as int)
let managed = ffi_to_string(raw)  // convert a returned char* into a managed Varian string
```

`@ffi("path/to/lib.so", "symbol")` on a bodyless `fn` declaration binds directly to a C
shared library symbol via `libffi`. Supported FFI parameter/return types: `c_int`,
`c_double`, `c_float`, `c_char`, `ptr`. Use the global `ffi_to_string(ptr)` to convert a
returned `char*` into a real Varian string when a C function hands back text.

## `errors` — error diagnostics and hints

The `errors` module is a native module that helps turn any raw error string or error struct into developer-friendly diagnostic category and actionable hints.

```varian
// Match standard runtime error messages:
let err = "Undefined variable 'foo'"
print(errors.explain(err))
// Prints:
// x UndefinedName
//   what: Undefined variable 'foo'
//   fix:  Declare it first with `let name = ...`, or check the spelling/scope.

print(errors.kind(err))                 // "UndefinedName"
print(errors.is(err, "UndefinedName"))  // true

// Make your own custom error structure:
let my_err = errors.make("PaymentFailed", "card declined", "ask the user to try another card")
print(my_err.kind)     // "PaymentFailed"
print(my_err.message)  // "card declined"
print(my_err.hint)     // "ask the user to try another card"
```

- `errors.explain(err)`: returns a multi-line friendly summary of the error.
- `errors.kind(err)`: returns a short category name (e.g. `UndefinedName`, `NoSuchField`, `DivByZero`, `IndexOutOfBounds`, `TypeMismatch`, `WrongArgCount`, etc.).
- `errors.is(err, kind_string)`: returns `true` if `err`'s kind matches `kind_string`.
- `errors.make(kind, message, hint)`: returns a structured Error struct containing `kind`, `message`, and `hint`.

