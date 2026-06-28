# Windows Compatibility Research Report

**Date:** 2026-06-27  
**Project:** Varian Programming Language  
**Scope:** Full runtime execution parity with Linux/WSL on Windows

---

## 1. Executive Summary

The current `vn.exe` on Windows is **LSP-only** (`src/main_win32.c` calls only `lsp_main()`). The full-featured binary (`src/main.c`) -- which provides REPL, file execution, Lumen dev server, fmt, test, lint, build, add/remove packages, new, dev, and compile -- is **excluded from the Windows build** (Makefile line 135: SRCS filter-out).

Additionally, the entire HTTP module (`src/lib_http.c`) is disabled on Windows (Makefile line 29: USE_HTTP=0 on Windows), which also disables the HTTP server (http.serve, http.serve_tls, http.serve_with_routes, raw socket I/O functions).

The core blocking issues fall into four categories:
1. **POSIX-only process management** -- fork(), exec(), waitpid(), kill(), signal() used by the Lumen dev server
2. **Linux /proc filesystem dependency** -- readlink("/proc/self/exe") used in multiple critical locations
3. **Missing epoll/I/O multiplexing layer** -- include/platform_io.h has linux and APPLE branches but hits #error on Windows
4. **POSIX-only networking/sockets** -- BSD socket API, sys/uio.h (writev), sys/socket.h, netinet/in.h, netdb.h used directly

---

## 2. Current Build System Analysis (Makefile)

| Aspect | Linux | Windows |
|--------|-------|---------|
| HTTP module | USE_HTTP=1 (linked with -luring) | USE_HTTP=0 (disabled, VN_NO_HTTP defined) |
| Main source | src/main.c | src/main_win32.c (LSP only) |
| Regex library | libc regex | Static deps/libtre.a (built from source) |
| Dynamic linking | -ldl | Uses LoadLibraryA via ifdef _WIN32 in ffi.c |
| Linker flags | -lm -lcurl -lcrypto -lssl -lpthread -ldl -luring | -lws2_32 -lwinmm -lcrypt32 + bundled static libs |
| Toolchain | system gcc | MSYS2/MinGW-w64 gcc |
| Deploy | single binary | binary + 21 bundled DLLs |
| Build deps | system packages | deps/ dir + DEPS_DIR env var |
| Temp path | /tmp | /tmp (assumes MSYS2 environment) |

**Key observations:**
- The Windows build depends entirely on an MSYS2/MinGW-w64 environment. It bundles 21 MSYS2 DLLs alongside the binary.
- The DEPS_DIR defaults to C:/deps -- a hardcoded path.
- The tre regex library is used on Windows instead of libc regex, built via deps/build_tre.sh which uses /tmp (assumes MSYS2).
- Only main_win32.c is compiled on Windows; main.c is explicitly filtered out.

---

## 3. File-by-File POSIX API Audit

### 3.1 include/platform_io.h -- CRITICAL BLOCKER

```
Line 98-99: #else
Line 99:     #error "Unsupported platform"
```

- Defines epoll (linux) and kqueue (APPLE) branches.
- **No Windows branch exists.**
- io_uring structs/functions are stubbed for macOS but NOT for Windows.
- Constants SOCK_NONBLOCK and SOCK_CLOEXEC are only defined for macOS fallback.
- This file is included by lib_http.c, so it would prevent compilation even if HTTP were enabled.
- **Fix needed:** Add a `#elif defined(_WIN32)` branch with Winsock/IOCP equivalent types, or at minimum stub out epoll/io_uring.

### 3.2 src/lib_http.c -- Entire file excluded on Windows

| Category | APIs |
|----------|------|
| Sockets | socket(), bind(), listen(), accept(), connect(), send(), recv(), setsockopt(), getpeername() |
| Socket constants | AF_INET, SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR, SO_REUSEPORT, IPPROTO_TCP, TCP_NODELAY, INADDR_ANY |
| Socket types | struct sockaddr_in, socklen_t, struct iovec |
| I/O multiplexing | epoll_create1(), epoll_ctl(), epoll_wait(), io_uring_queue_init(), io_uring_prep_*() |
| File control | fcntl(), F_GETFL, F_SETFL, O_NONBLOCK |
| Threading | pthread_create(), pthread_detach(), pthread_mutex_t, pthread_mutex_*() |
| Signals | signal(), SIGINT, SIGTERM, sig_atomic_t |
| Time | clock_gettime(), CLOCK_MONOTONIC_COARSE, gettimeofday() |
| I/O vector | writev() (from sys/uio.h) |
| File descriptor | close() (should be closesocket() on Windows) |
| Error handling | errno, strerror(errno), EAGAIN, EWOULDBLOCK |
| TLS | __thread storage class (line 1928) |

**Architecture note:** The HTTP server uses cooperative multitasking (round-robin task scheduler in vm.c) combined with epoll/io_uring for I/O. A Windows port would need to replace epoll/io_uring with IOCP or emulated epoll via WSAPoll/select().

### 3.3 src/main.c -- Not compiled on Windows

| Line | API | Purpose | Windows Equivalent |
|------|-----|---------|-------------------|
| 218, 446, 2135 | readlink("/proc/self/exe") | Locate own exe path | GetModuleFileNameW() |
| 436 | fork() | Spawn Lumen dev server child | CreateProcess() |
| 449 | execl() | Exec server child with args | CreateProcess() |
| 450 | _exit() | Exit child after failed exec | ExitProcess() |
| 438, 443 | setenv() | Set env vars in child | SetEnvironmentVariable() |
| 469 | isatty(STDOUT_FILENO) | Check if terminal | _isatty(_fileno(stdout)) |
| 474 | clock_gettime(CLOCK_MONOTONIC) | Benchmark timing | QueryPerformanceCounter() |
| 613 | nanosleep() | Poll sleep in dev server | Sleep() |
| 617, 625 | waitpid() | Wait for child server | WaitForSingleObject() |
| 624 | kill(child, SIGTERM) | Kill child server | TerminateProcess() |
| 1664 | mkstemp() | Temp file for diff | _mktemp_s() or GetTempFileNameW() |
| 1667 | write() | Write to temp fd | _write() |
| 1669 | close() | Close temp fd | _close() |
| 1673 | unlink() | Remove temp file | _unlink() |
| 2064 | chmod(target_path, 0755) | Make executable | Not needed on Windows |
| Various | opendir, readdir, DIR, dirent | Directory listing | FindFirstFileW() / FindNextFileW() |
| Various | stat(), struct stat, S_ISDIR() | File info | _stat() / GetFileAttributesW() |
| Various | mkdir(path, mode) | Create directory | _mkdir(path) (no mode param) |

### 3.4 src/vm.c -- Used on all platforms (core interpreter)

| Line | API | Windows Issue |
|------|-----|---------------|
| 9 | #include <sys/time.h> | MinGW provides this; MSVC does not |
| 11 | #include <unistd.h> | MinGW provides this; MSVC does not |
| 12 | #include <pthread.h> | MinGW provides pthreads; MSVC needs _beginthreadex() |
| 3502, 3622 | gettimeofday() | MinGW provides; else GetSystemTimeAsFileTime() |
| 3589 | nanosleep() | MinGW provides; else Sleep() |
| 3786 | pthread_mutex_t, PTHREAD_MUTEX_INITIALIZER | MinGW provides; else CRITICAL_SECTION |

vm.c is the core interpreter runtime. The dispatch table mutex (line 3786) is the only synchronization primitive.

### 3.5 src/lib_io.c -- Used on all platforms

- #include <unistd.h> -- MinGW provides this
- #include <sys/stat.h> -- MinGW provides this
- #include <dirent.h> (behind ifdef _WIN32) -- MinGW also has this
- mkdir() -- Windows _mkdir() takes no mode param. The ifdef guard is in main.c but NOT in lib_io.c
- stat() -- works in MinGW; MSVC needs _stat()
- opendir/readdir/closedir/DIR/dirent -- MinGW provides dirent.h

### 3.6 src/pkg_manager.c -- Used on all platforms

- readlink("/proc/self/exe") at line 1489 -- Linux-specific.
- The ifdef _WIN32 block (lines 9-16) defines a readlink() stub returning -1, but this breaks executable path resolution on Windows.
- unlink() -- MinGW provides; MSVC needs _unlink()
- Includes: sys/stat.h, dirent.h, unistd.h -- all MinGW-compatible

### 3.7 src/ffi.c -- Good Windows support

- Has a full ifdef _WIN32 block mapping dlopen->LoadLibraryA, dlsym->GetProcAddress, dlclose->FreeLibrary
- Still uses pthread_mutex_t for the library cache lock (needs MinGW)

### 3.8 src/lib_env.c -- Partial Windows support

- Has ifdef _WIN32 mapping setenv()->_putenv_s()
- Otherwise portable (uses getenv(), fopen(), fgets())

### 3.9 src/lib_smtp.c -- No Windows support

- Includes: unistd.h, sys/socket.h, netdb.h -- MinGW provides via Winsock wrappers
- socket(), connect(), close(), send(), recv(), getaddrinfo(), freeaddrinfo() -- MinGW maps to Winsock
- Would need WSAStartup()/WSACleanup() without MinGW

### 3.10 src/lib_task.c -- Minor issue

- #include <sys/time.h> + gettimeofday() -- MinGW handles this
- Otherwise portable (no OS threading syscalls -- cooperative VM-level task scheduler)

### 3.11 Other files with POSIX dependencies

| File | POSIX APIs | Notes |
|------|-----------|-------|
| src/lint.c | dirent.h, sys/stat.h, opendir/readdir | Needs MinGW or shims |
| src/test_runner.c | dirent.h, opendir/readdir, struct stat | Needs MinGW or shims |
| src/parser.c | dirent.h, opendir/readdir | Only for use directive resolution |
| src/lib_python.c | unistd.h | MinGW |
| src/lib_time.c | sys/time.h, gettimeofday() | MinGW |
| src/lib_postgres.c | Uses libpq (portable) | MinGW via bundled DLLs |
| src/lib_redis.c | Uses hiredis (portable) | MinGW via bundled DLLs |
| src/lib_sqlite.c | Uses sqlite3 (portable) | MinGW via bundled DLLs |

---

## 4. Existing _WIN32 Guards (Complete Inventory)

| File | Lines | What It Does |
|------|-------|-------------|
| src/main.c | 16-19 | #define mkdir(path, mode) _mkdir(path) + #include <direct.h> |
| src/main.c | 2062-2066 | #ifndef _WIN32 ... chmod() ... #endif |
| src/main_win32.c | 7-10, 15-19 | #include <fcntl.h>, <io.h>; _setmode() for binary stdio |
| src/pkg_manager.c | 9-16 | Stub readlink() to return -1 |
| src/lib_io.c | 6-10 | #include <direct.h>, <io.h>, <dirent.h> |
| src/lib_env.c | 6-8 | #define setenv(key,val,overwrite) _putenv_s(key,val) |
| src/ffi.c | 7-17 | Full dlopen/dlsym/dlclose -> LoadLibrary/GetProcAddress/FreeLibrary |

**Notable absences:** No _WIN32 guards in vm.c, lib_http.c, lib_smtp.c, lib_task.c, lib_time.c, lib_python.c, lint.c, test_runner.c, parser.c, or platform_io.h.

---

## 5. Blocking Gaps by Severity

### Level 1: Critical (prevents compilation or core functionality)

| # | Gap | Files Affected | Windows Equivalent |
|---|-----|----------------|-------------------|
| 1 | #error in platform_io.h | platform_io.h, lib_http.c | Add _WIN32 branch with IOCP/select epoll shim |
| 2 | fork()/exec()/waitpid()/kill() in Lumen dev server | main.c (lines 436-453) | CreateProcess() + WaitForSingleObject() + TerminateProcess() |
| 3 | readlink("/proc/self/exe") in 3+ locations | main.c, pkg_manager.c | GetModuleFileNameW() |
| 4 | main.c entirely excluded from build | Makefile (line 135) | Add _WIN32 guards + Windows code paths |
| 5 | lib_http.c entirely excluded | Makefile (line 29, USE_HTTP=0) | Add Winsock/IOCP implementation |

### Level 2: High (non-compilation with MSVC; works with MinGW)

| # | Gap | Files Affected | Notes |
|---|-----|----------------|-------|
| 6 | <unistd.h> in 8 files | vm, lib_io, pkg_manager, lib_smtp, lib_python, lib_http, main, lib_task | MinGW provides it |
| 7 | <sys/time.h> / gettimeofday() | vm, lib_http, lib_task, lib_time | MinGW provides; else GetSystemTimeAsFileTime() |
| 8 | <sys/stat.h> / stat() | lib_io, main, pkg_manager, lint, test_runner | MinGW provides; MSVC uses _stat() |
| 9 | <dirent.h> / opendir/readdir | lib_io, main, pkg_manager, lint, test_runner, parser | MinGW provides; else FindFirstFile/FindNextFile |
| 10 | mkdir() with mode param | lib_io.c (line 144), main.c | #define mkdir(p,m) _mkdir(p) needed in lib_io.c |
| 11 | pthread_mutex_t / pthread_create() | vm.c, ffi.c, lib_http.c | MinGW provides pthreads |
| 12 | nanosleep() | vm.c, main.c | MinGW provides; else Sleep() |
| 13 | clock_gettime() | lib_http.c, main.c | MinGW provides; else QueryPerformanceCounter() |

### Level 3: Medium (BSD socket API)

| # | Gap | Files Affected | Notes |
|---|-----|----------------|-------|
| 14 | socket/bind/listen/accept/connect | lib_http.c, lib_smtp.c | MinGW wraps to Winsock; needs WSAStartup() |
| 15 | send()/recv() | lib_http.c, lib_smtp.c | MinGW wraps to WSASend/WSARecv |
| 16 | close() on sockets | lib_http.c | Should be closesocket() |
| 17 | fcntl()/F_SETFL/O_NONBLOCK | lib_http.c | ioctlsocket() + FIONBIO |
| 18 | getaddrinfo()/freeaddrinfo() | lib_smtp.c | Winsock provides these |
| 19 | errno/EAGAIN/EWOULDBLOCK | lib_http.c | WSAGetLastError()/WSAEWOULDBLOCK |
| 20 | writev() / <sys/uio.h> | lib_http.c (line 631) | WSASend() with scatter/gather |
| 21 | signal()/SIGINT/SIGTERM | lib_http.c | SetConsoleCtrlHandler() |
| 22 | __thread (TLS) | lib_http.c (line 1928) | __declspec(thread) or TlsGetValue() |

### Level 4: Low (cosmetic / environment)

| # | Gap | Files Affected | Notes |
|---|-----|----------------|-------|
| 23 | ANSI escape codes | main.c (lines 456-465) | Windows 10+ supports via virtual terminal |
| 24 | /tmp path assumption | Makefile, main.c | GetTempPathW() on Windows |
| 25 | DEPS_DIR = C:/deps | Makefile | Hardcoded; should be configurable |
| 26 | chmod() in build | main.c (line 2064) | Already has #ifndef _WIN32 guard |
| 27 | isatty(STDOUT_FILENO) | main.c (line 469) | _isatty(_fileno(stdout)) |

---

## 6. Step-by-Step Remediation Plan

### Phase 1: Core Platform Abstraction (unblocks compilation)

**1. Fix include/platform_io.h**
- Add a #elif defined(_WIN32) branch
- Provide epoll shim using WSAPoll() or select() (simple but lower perf)
- Stub io_uring structs/functions to always return failure (graceful fallback, already done for macOS)
- Define SOCK_NONBLOCK and SOCK_CLOEXEC as 0 on Windows

**2. Add a new include/platform_compat.h**
- Provide macros/typedefs for:
  - gettimeofday() -> GetSystemTimeAsFileTime() conversion
  - nanosleep() -> Sleep() conversion
  - clock_gettime() -> QueryPerformanceCounter() conversion
  - readlink() -> stub returning -1 or GetModuleFileName
  - mkdir(p,m) -> _mkdir(p)
  - strcasecmp() -> _stricmp()
  - ssize_t -> SSIZE_T or int
  - isatty() -> _isatty()
  - STDOUT_FILENO -> _fileno(stdout)

**3. Fix src/main.c for Windows**
- Replace readlink("/proc/self/exe") with GetModuleFileNameW(NULL, ...) under _WIN32
- Replace fork()/exec()/waitpid()/kill() Lumen dev server pattern with CreateProcess() + WaitForSingleObject() + TerminateProcess()
- Alternatively: fall back to the in-process blocking serve (already documented at line 591)
- Replace opendir/readdir with FindFirstFileW/FindNextFileW (or use MinGW dirent.h)
- Replace mkstemp()/write()/close() with GetTempFileNameW() + _write() + _close()
- Replace clock_gettime() with QueryPerformanceCounter()
- Replace nanosleep() with Sleep()

### Phase 2: Networking (re-enable HTTP on Windows)

**4. Fix src/lib_http.c for Windows**
- Add ifdef _WIN32 guard including winsock2.h and windows.h
- Call WSAStartup() at initialization, WSACleanup() at cleanup
- Replace close(fd) with closesocket(fd)
- Replace fcntl(F_SETFL, O_NONBLOCK) with ioctlsocket(FIONBIO)
- Replace errno/EAGAIN with WSAGetLastError()/WSAEWOULDBLOCK
- Replace writev() with WSASend() (scatter/gather)
- Replace epoll with WSAPoll() or select() for simple working implementation
- Replace io_uring with fallback (no Linux io_uring on Windows)
- Replace __thread with __declspec(thread) or TlsGetValue()
- Replace signal() with SetConsoleCtrlHandler()
- Replace SO_REUSEPORT (unavailable on Windows) with SO_REUSEADDR only

**5. Re-enable USE_HTTP on Windows in Makefile** (once lib_http.c compiles)

### Phase 3: Package Manager & Tooling

**6. Fix src/pkg_manager.c for Windows**
- Replace readlink("/proc/self/exe") with GetModuleFileNameW()
- Ensure _WIN32 guards for unlink() -> _unlink() for MSVC

**7. Fix src/vm.c for MSVC compatibility** (if targeting non-MinGW)
- Replace <sys/time.h> + gettimeofday() with Windows equivalents
- Replace <unistd.h> (not needed for MSVC)
- Replace pthread_mutex_t with CRITICAL_SECTION
- Replace nanosleep() with Sleep()

**8. Fix src/lib_io.c**
- Add #define mkdir(path, mode) _mkdir(path) macro (currently only in main.c)
- Add stat() -> _stat() mapping for MSVC

**9. Fix src/lib_smtp.c** -- Add Winsock headers and WSAStartup()

**10. Fix src/lib_task.c, src/lib_time.c** -- Add gettimeofday() shim for MSVC

### Phase 4: Build System

**11. Fix Makefile for Windows**
- Remove hardcoded C:/deps path (use relative paths or autodetection)
- Use Windows-native temp paths instead of /tmp
- Add properly configured USE_HTTP=1 for Windows once lib_http.c is ported
- Optionally support MSVC (cl.exe) as alternative to MinGW

**12. Reduce DLL dependency burden**
- Explore static linking for libcurl, openssl, etc.
- Add -static-libgcc -static-libstdc++ to eliminate libgcc_s_seh-1.dll and libstdc++-6.dll

### Phase 5: Testing & CI

**13. Enable test runner on Windows** (src/test_runner.c)
- Should work once Phase 1 basic compatibility is in place (uses stdio + dirent)

**14. Add Windows CI**
- Add GitHub Actions workflow for Windows using MSYS2 or Cygwin

---

## 7. Effort Estimate

| Phase | Complexity | Files Touched | Estimated Effort (days) |
|-------|-----------|---------------|--------------------------|
| 1 - Core Platform Abstraction | Medium | 6 files | 2-3 |
| 2 - Networking (lib_http) | High | 2 files | 5-10 |
| 3 - Package Manager & Tooling | Medium | 4-5 files | 2-3 |
| 4 - Build System | Low | 1 file | 1 |
| 5 - Testing & CI | Low | 2 files | 1 |
| **Total** | | | **~11-18** |

**Shortcut with MinGW:** If MinGW is accepted as the permanent Windows toolchain (instead of targeting MSVC natively), Phases 1-4 shrink dramatically -- MinGW already provides most POSIX compatibility headers. The remaining truly missing pieces are:
1. fork()/exec()/waitpid()/kill() -- replace with CreateProcess()/WaitForSingleObject()/TerminateProcess()
2. readlink("/proc/self/exe") -- replace with GetModuleFileNameW()
3. epoll/io_uring -- stub or replace with select()/WSAPoll()/WSAEventSelect()
4. mkstemp() -- replace with GetTempFileNameW()
5. writev()/<sys/uio.h> -- replace with WSASend() or manual loop

With the MinGW shortcut, total effort reduces to approximately **5-8 days**.

---

## 8. Current Windows vs Linux Feature Matrix

| Feature | Linux | Windows (Current) | Windows (Target) |
|---------|-------|-------------------|------------------|
| REPL | Yes | **No** | Yes |
| vn run <file> | Yes | **No** | Yes |
| vn fmt | Yes | **No** | Yes |
| vn test | Yes | **No** | Yes |
| vn lint | Yes | **No** | Yes |
| vn build | Yes | **No** | Yes |
| vn add/remove/install | Yes | **No** | Yes |
| vn dev (Lumen dev server) | Yes | **No** | Yes (in-process fallback) |
| vn new (scaffold) | Yes | **No** | Yes |
| vn compile (AOT) | Yes | **No** | Yes |
| vn lsp | Yes | **Yes** | Yes |
| http.get/http.post | Yes | **No** (USE_HTTP=0) | Yes |
| http.serve (server) | Yes | **No** (USE_HTTP=0) | Yes |
| WebSocket/SSE | Yes | **No** | Yes |
| PostgreSQL | Yes | **Yes** (via DLLs) | Yes |
| SQLite | Yes | **Yes** | Yes |
| Redis | Yes | **Yes** | Yes |
| FFI | Yes | **Yes** (via LoadLibrary) | Yes |
| SMTP | Yes | **No** | Yes |
| Cluster workers | Yes (pthread) | **No** | Yes (via MinGW) |
| AOT native binary compilation | Yes | **No** | Yes (cross-compile) |
