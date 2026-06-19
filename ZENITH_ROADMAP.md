# Zenith Framework Roadmap

Zenith is the official web framework for Varian. To reach production-level parity with frameworks like FastAPI, Express, or Spring Boot, we must implement a comprehensive set of backend features. 

Following Varian's "Cheat Code Architecture," we categorize every feature by how it will be implemented:
- **Pillaged**: Bound to existing, battle-tested C libraries via zero-overhead `@ffi`.
- **Bridged**: Proxied to Python via `python.run` (used strictly for massive ecosystems where rewriting is impossible).
- **Native**: Written in 100% pure Varian code leveraging Tasks, Actors, and Structs.

---

## 1. Core Web Server (`vn.http` & Zenith)
* **HTTP Engine (POSIX Sockets)**: **Pillaged**. C-level non-blocking sockets integrated deeply into the Varian spin-yield scheduler.
* **Dynamic Routing (Path Params `/users/:id`)**: **Native**. Regex-based compilation of route paths in Varian (`zenith.vn`).
* **Middleware & Request Chaining**: **Native**. First-class `app.use(fn(req, next))` functions in Zenith.
* **JSON Parsing & Validation**: **Native**. Using Varian's structural typing and `@validate` to parse JSON into `VAL_DICT` and Structs.
* **WebSockets & SSE**: **Pillaged/Native**. C-level socket upgrade, handled natively by Varian Channels for real-time data passing.

## 2. Validation & Sanitization (`vn.validate` & `vn.sanitize`)
* **Data Validation (`vn.validate`)**: **Native**. Using the `@validate` decorator on Varian Structs to automatically enforce schema constraints (replacing Pydantic/Zod).
* **Input Sanitization (`vn.sanitize`)**: **Native/Pillaged**. C-level string scrubbing (or native Varian regex) to strip malicious HTML/XSS tags and sanitize SQL inputs.

## 3. Database & ORM (`vn.db`)
* **PostgreSQL Driver**: **Pillaged**. Direct `@ffi` bindings to `libpq`.
* **SQLite Driver**: **Pillaged**. Direct `@ffi` bindings to `libsqlite3`.
* **Redis Client**: **Pillaged**. Direct `@ffi` bindings to `hiredis`.
* **Query Builder & ORM**: **Native**. Using Varian's `comptime` feature to generate safe, parameterized SQL queries at compile-time (bypassing the slow runtime reflection used by SQLAlchemy).
* **Migrations**: **Native**. A CLI tool to parse and execute `.sql` files natively.

## 4. Authentication & Cryptography (`vn.auth`)
* **Hashing (bcrypt/argon2)**: **Pillaged**. Bound to `libbcrypt` or `libsodium` via C FFI.
* **JWT (JSON Web Tokens)**: **Native/Pillaged**. `OpenSSL` via C for the raw HMAC/RSA cryptography, but the JWT parsing/verification logic written in pure Varian.
* **OAuth2 (Google, GitHub)**: **Native**. Standard HTTP calls via Varian's `vn.http.client`—no bloated vendor SDKs needed.
* **Session Management**: **Native**. Managed in-memory via Varian Actors or backed by Redis (`vn.db`).

## 5. Background Jobs & Concurrency (`vn.queue`)
* **Cron Jobs & Scheduled Tasks**: **Native**. Powered entirely by Varian `Task` loops with `task.sleep()`. No APScheduler or Celery needed.
* **Task Queues**: **Native**. Using Varian `Channel` ring-buffers for in-memory queues, or `vn.db` for Redis-backed persistence.

## 6. Security (`vn.shield`)
* **Rate Limiting**: **Native**. Implemented as Zenith Middleware using Varian Actors (Token Bucket algorithm) for thread-safe IP tracking.
* **CORS & CSRF Protection**: **Native**. Handled securely via Zenith Middleware.
* **Secret Scanning**: **Native**. Compile-time security scanning blocking builds if API keys are found.

## 7. Cloud Storage (`vn.storage`)
* **S3 / GCS / R2 Uploads**: **Bridged**. Proxied to Python (`boto3`) to avoid writing complex AWS Signature V4 logic manually in C.
* **Local File System**: **Pillaged**. Standard POSIX `<stdio.h>` file streams.

## 8. Math & Analytics (`vn.math`)
* **Core Math (Trig, Logs, Stats)**: **Pillaged**. Standard C `<math.h>` functions exposed to Varian natively.
* **Complex Mathematics (Matrices, Calculus)**: **Bridged**. Proxied to Python (`numpy`, `scipy`) using `vn wrap` for data science and analytics workloads.

## 9. Email & Communication (`vn.mail`)
* **SMTP Protocol**: **Native**. Raw TCP socket implementation using `vn.net`.
* **Resend / SendGrid APIs**: **Native**. Direct API calls via `vn.http.client`.
* **Email Templating**: **Native**. Rendered using Varian's built-in `.vhtml` server-side components (replacing Jinja2).

## 10. Artificial Intelligence (`vn.ai`)
* **LLM Orchestration (LangChain/LlamaIndex)**: **Bridged**. Proxied to Python.
* **Embeddings & PyTorch**: **Bridged**. Proxied to Python (`sentence-transformers`).
* **OpenAI / Anthropic Calls**: **Native**. Standard HTTP requests natively in Zenith.

## 11. Observability & Telemetry (`vn.observe`)
* **Structured Logging**: **Native**. JSON-formatted logs written to stdout.
* **Metrics (Prometheus)**: **Native**. An Actor that collects request counts and exposes a `/metrics` Zenith route.
* **Distributed Tracing (OpenTelemetry)**: **Pillaged/Native**. C OpenTelemetry SDK bindings, automatically injecting span IDs into the Zenith request context.
