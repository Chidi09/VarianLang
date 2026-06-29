# deps/

Third-party build dependencies that are **fetched and built on demand**, not
vendored into git.

- `build_tre.sh` — bootstraps the static [TRE](https://github.com/laurikari/tre)
  regex library (`libtre.a` + headers) used only by the **Windows/MinGW** build,
  since MinGW lacks POSIX `<regex.h>`. Linux and macOS use libc's regex and skip
  this entirely. `make` runs it automatically via the `deps/libtre.a` target.

Everything else under `deps/` (the unpacked `tre-0.9.0/` source, `libtre.a`) is
generated and git-ignored.
