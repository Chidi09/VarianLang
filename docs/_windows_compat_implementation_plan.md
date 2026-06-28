# Implementation Plan for Windows Compatibility Shims

We want to add basic `#ifdef _WIN32` shims to unblock Windows execution without tearing apart the architecture. You will modify the existing C files to provide direct Windows API equivalents where POSIX APIs are used. Do not rewrite large subsystems; just add the #ifdef branches.

## Tasks

### 1. `include/platform_io.h` (I/O Multiplexing)
- Add a new `#elif defined(_WIN32)` block.
- In this block, implement the `epoll` layer using either `WSAPoll` or `select()` so the HTTP module compiles.
- Map socket descriptor types appropriately (e.g. `SOCKET` vs `int`).
- Stub the `io_uring` structs and functions to safely do nothing or return failure, just as they are currently stubbed for Apple/macOS.
- Define `SOCK_NONBLOCK` and `SOCK_CLOEXEC` to 0.

### 2. `src/main.c` (Process & Env Management)
- **`readlink("/proc/self/exe")`**: Add `#ifdef _WIN32` block replacing this with `GetModuleFileNameA(NULL, buffer, size)`.
- **Lumen dev server (`fork`/`exec`/`waitpid`/`kill`)**: Add an `#ifdef _WIN32` branch using `CreateProcessA`, `WaitForSingleObject`, and `TerminateProcess`.
- **`clock_gettime` / `nanosleep`**: Add `#ifdef _WIN32` fallbacks using `QueryPerformanceCounter` and `Sleep()`.
- **`mkstemp` / `write` / `close` / `unlink`**: Add `#ifdef _WIN32` fallbacks using `GetTempFileNameA`, `_write`, `_close`, and `_unlink`.

### 3. `src/lib_http.c` & `src/lib_smtp.c` (Networking)
- Add `#ifdef _WIN32` blocks to include `<winsock2.h>` and `<ws2tcpip.h>`.
- Add a mechanism (perhaps in `main.c` or module init) to call `WSAStartup` once.
- Replace POSIX socket constants (`O_NONBLOCK` via `fcntl`) with `ioctlsocket(..., FIONBIO, ...)`.
- Wrap socket cleanup in `#ifdef _WIN32` using `closesocket()` instead of `close()`.
- Handle `errno` vs `WSAGetLastError()` explicitly where `EAGAIN` or `EWOULDBLOCK` are checked.

### 4. `Makefile`
- On Windows, ensure `src/main.c` is compiled (do not filter it out).
- Re-enable the HTTP module (`USE_HTTP=1`) on Windows so that it is actually compiled and linked against Winsock (`-lws2_32`).

Be extremely precise. We want to shim the missing pieces cleanly so it compiles and runs without needing a heavy MSVC port. Maintain MinGW compatibility as the primary Windows toolchain.
