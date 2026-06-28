#ifndef NATIVE_DOCS_H
#define NATIVE_DOCS_H

#include <stddef.h>

typedef struct {
    const char *name;
    const char *signature;
    const char *description;
    const char *example;
} NativeDocEntry;

static NativeDocEntry native_docs[] = {
    // paginate
    {"paginate", "fn paginate(items, page, per_page)", "Paginates a collection of items.", "```varian\nlet result = paginate(users, 1, 10)\nprint(result.items)\n```"},
    {"paginate.items", "items", "The items on the current page.", ""},
    {"paginate.total", "total", "The total count of items.", ""},
    {"paginate.page", "page", "The current page number.", ""},
    {"paginate.per_page", "per_page", "The number of items per page.", ""},
    {"paginate.total_pages", "total_pages", "The total number of pages.", ""},

    // migrate
    {"new_migrator", "fn new_migrator(conn)", "Creates a new Migrator instance.", "```varian\nlet m = new_migrator(conn)\n```"},
    {"Migrator.register", "fn register(self, name, up_sql, down_sql)", "Registers a migration with both up and down SQL.", "```varian\nm.register(\"001_init\", \"CREATE TABLE users (id INTEGER)\", \"DROP TABLE users\")\n```"},
    {"Migrator.register_up", "fn register_up(self, name, up_sql)", "Registers a migration with only up SQL.", "```varian\nm.register_up(\"002_add_idx\", \"CREATE INDEX idx_users ON users(id)\")\n```"},
    {"Migrator.up", "fn up(self)", "Applies all pending migrations.", "```varian\nm.up()\n```"},
    {"Migrator.down", "fn down(self, steps)", "Rolls back a specified number of migrations.", "```varian\nm.down(1)\n```"},
    {"Migrator.applied", "fn applied(self)", "Returns a list of applied migration names.", "```varian\nlet done = m.applied()\n```"},
    {"Migrator.pending", "fn pending(self)", "Returns a list of pending migration names.", "```varian\nlet active = m.pending()\n```"},

    // queue
    {"new_queue", "fn new_queue(conn)", "Creates a new Redis-backed background job queue.", "```varian\nlet q = new_queue(redis.connect(\"localhost\", 6379))\n```"},
    {"new_queue_with_prefix", "fn new_queue_with_prefix(conn, prefix)", "Creates a new Redis-backed background job queue with a prefix.", "```varian\nlet q = new_queue_with_prefix(conn, \"jobs\")\n```"},
    {"Queue.push", "fn push(self, job_name, payload)", "Pushes a new job into the queue.", "```varian\nq.push(\"send_email\", { to: \"user@example.com\" })\n```"},
    {"Queue.push_delay", "fn push_delay(self, job_name, payload, delay_seconds)", "Pushes a new job into the queue to be executed after a delay.", "```varian\nq.push_delay(\"send_email\", { to: \"user@example.com\" }, 60)\n```"},
    {"Queue.work", "fn work(self, handlers)", "Starts the queue worker loop with a struct/map of job handlers.", "```varian\nq.work({\n    send_email: |payload| { print(\"Sending email to\", payload.to) }\n})\n```"},

    // ws
    {"upgrade_websocket", "fn upgrade_websocket(req)", "Upgrades an HTTP request to a WebSocket connection.", "```varian\nlet ws = upgrade_websocket(req)\n```"},
    {"WebSocket.read", "fn read(self)", "Reads the next frame from the WebSocket.", "```varian\nlet msg = ws.read()\n```"},
    {"WebSocket.write", "fn write(self, text)", "Writes a text frame to the WebSocket.", "```varian\nws.write(\"Hello client!\")\n```"},
    {"WebSocket.close", "fn close(self)", "Closes the WebSocket connection.", "```varian\nws.close()\n```"},

    // validate
    {"validate", "fn validate(obj, schema)", "Validates a struct or map against a schema/struct rules.", "```varian\nlet errors = validate(user, UserSchema)\n```"},

    // storage
    {"new_disk_storage", "fn new_disk_storage(root_dir)", "Creates a disk-backed storage driver.", "```varian\nlet store = new_disk_storage(\"./uploads\")\n```"},
    {"DiskStorage.put", "fn put(self, path, content)", "Saves a file to disk storage.", "```varian\nstore.put(\"hello.txt\", \"Hello World!\")\n```"},
    {"DiskStorage.get", "fn get(self, path)", "Retrieves a file's content from disk storage.", "```varian\nlet text = store.get(\"hello.txt\")\n```"},
    {"DiskStorage.exists", "fn exists(self, path)", "Checks if a file exists in storage.", "```varian\nif store.exists(\"hello.txt\") { ... }\n```"},
    {"DiskStorage.delete", "fn delete(self, path)", "Deletes a file from storage.", "```varian\nstore.delete(\"hello.txt\")\n```"},

    // zenith
    {"new_zenith", "fn new_zenith()", "Creates a new Zenith web application instance.", "```varian\nlet app = new_zenith()\n```"},
    {"ZenithApp.get", "fn get(self, path, handler, summary, meta)", "Registers a GET route handler.", "```varian\napp.get(\"/\", |req| { return \"Hello\" })\n```"},
    {"ZenithApp.post", "fn post(self, path, handler, summary, meta)", "Registers a POST route handler.", "```varian\napp.post(\"/users\", |req| { ... })\n```"},
    {"ZenithApp.put", "fn put(self, path, handler, summary, meta)", "Registers a PUT route handler.", "```varian\napp.put(\"/users/:id\", |req| { ... })\n```"},
    {"ZenithApp.delete", "fn delete(self, path, handler, summary, meta)", "Registers a DELETE route handler.", "```varian\napp.delete(\"/users/:id\", |req| { ... })\n```"},
    {"ZenithApp.add_middleware", "fn add_middleware(self, handler)", "Registers a middleware handler to the application.", "```varian\napp.add_middleware(|req, next| { return next(req) })\n```"},
    {"ZenithApp.listen", "fn listen(self, port)", "Starts the Zenith application listening on a port.", "```varian\napp.listen(8080)\n```"},
    {"ZenithApp.listen_cluster", "fn listen_cluster(self, port, workers)", "Starts the Zenith application listening on a port with multiple worker processes.", "```varian\napp.listen_cluster(8080, 4)\n```"},
    {"ZenithApp.listen_tls", "fn listen_tls(self, port, cert_path, key_path)", "Starts the Zenith application with TLS support.", "```varian\napp.listen_tls(8443, \"cert.pem\", \"key.pem\")\n```"},

    // ratelimit
    {"new_rate_limiter", "fn new_rate_limiter(conn)", "Creates a new Redis rate limiter.", "```varian\nlet rl = new_rate_limiter(redis.connect(\"localhost\", 6379))\n```"},
    {"new_rate_limiter_with_prefix", "fn new_rate_limiter_with_prefix(conn, prefix)", "Creates a new Redis rate limiter with a key prefix.", "```varian\nlet rl = new_rate_limiter_with_prefix(conn, \"api\")\n```"},
    {"RateLimiter.check", "fn check(self, key, max_requests, window_seconds)", "Checks if key has exceeded request limits. Returns true if allowed.", "```varian\nif !rl.check(\"ip:127.0.0.1\", 60, 60) { ... }\n```"},
    {"RateLimiter.remaining", "fn remaining(self, key, max_requests)", "Returns the remaining requests allowed for key in current window.", "```varian\nlet rem = rl.remaining(\"ip:127.0.0.1\", 60)\n```"},
    {"RateLimiter.reset_in", "fn reset_in(self, key)", "Returns seconds until the rate limit window resets.", "```varian\nlet sec = rl.reset_in(\"ip:127.0.0.1\")\n```"},
    {"RateLimiter.reset", "fn reset(self, key)", "Resets the rate limiter for a key.", "```varian\nrl.reset(\"ip:127.0.0.1\")\n```"},

    // observe
    {"observe.metric_counter", "fn metric_counter(name, help)", "Registers a Prometheus counter metric.", "```varian\nlet c = observe.metric_counter(\"http_requests_total\", \"Total HTTP requests\")\n```"},
    {"observe.metric_gauge", "fn metric_gauge(name, help)", "Registers a Prometheus gauge metric.", "```varian\nlet g = observe.metric_gauge(\"memory_usage_bytes\", \"Current memory usage\")\n```"},
    {"observe.metric_histogram", "fn metric_histogram(name, help, buckets)", "Registers a Prometheus histogram metric.", "```varian\nlet h = observe.metric_histogram(\"http_latency_seconds\", \"Latency\", [0.1, 0.5, 1.0])\n```"},
    {"Counter.inc", "fn inc(self)", "Increments a metric counter.", "```varian\nc.inc()\n```"},
    {"Gauge.set", "fn set(self, value)", "Sets a metric gauge to a value.", "```varian\ng.set(42.0)\n```"},
    {"Histogram.observe", "fn observe(self, value)", "Records an observation in a metric histogram.", "```varian\nh.observe(0.25)\n```"},

    // mail
    {"mail.send_smtp", "fn send_smtp(host, port, from, to, subject, body)", "Sends an email using raw SMTP.", "```varian\nmail.send_smtp(\"localhost\", 1025, \"a@b.com\", \"c@d.com\", \"Hello\", \"Body\")\n```"},
    {"mail.send_resend", "fn send_resend(api_key, from, to, subject, body)", "Sends an email via Resend's API.", "```varian\nmail.send_resend(\"re_123\", \"a@b.com\", \"c@d.com\", \"Hello\", \"Body\")\n```"},

    // i18n
    {"new_i18n", "fn new_i18n(translations)", "Creates an i18n translation context.", "```varian\nlet tr = new_i18n({ en: { welcome: \"Welcome\" } })\n```"},
    {"I18n.t", "fn t(self, locale, key, params)", "Translates a key under a locale, replacing template params.", "```varian\nlet msg = tr.t(\"en\", \"welcome\", {})\n```"},

    // fetch
    {"fetch", "fn fetch(url)", "Constructs a fluent HTTP client builder for FetchRequest.", "```varian\nlet res = fetch(\"https://example.com\").get()\n```"},
    {"FetchRequest.header", "fn header(self, name, value)", "Sets an HTTP header for the fetch request.", "```varian\nreq.header(\"Content-Type\", \"application/json\")\n```"},
    {"FetchRequest.timeout", "fn timeout(self, ms)", "Sets request timeout in milliseconds.", "```varian\nreq.timeout(5000)\n```"},
    {"FetchRequest.retry", "fn retry(self, n)", "Sets the number of automatic retries on failure.", "```varian\nreq.retry(3)\n```"},
    {"FetchRequest.json", "fn json(self)", "Configures response body parsing as JSON.", "```varian\nlet data = fetch(url).json().get()\n```"},
    {"FetchRequest.get", "fn get(self)", "Executes the fetch request with GET method.", "```varian\nlet body = req.get()\n```"},
    {"FetchRequest.post", "fn post(self, body)", "Executes the fetch request with POST method sending a JSON-encoded body.", "```varian\nlet res = req.post({ name: \"Alice\" })\n```"},
    {"FetchRequest.put", "fn put(self, body)", "Executes the fetch request with PUT method sending a JSON-encoded body.", "```varian\nlet res = req.put({ name: \"Alice\" })\n```"},
    {"FetchRequest.delete", "fn delete(self)", "Executes the fetch request with DELETE method.", "```varian\nlet res = req.delete()\n```"},

    // feature
    {"new_flag_manager", "fn new_flag_manager(conn)", "Creates a Redis-backed feature flag manager.", "```varian\nlet fm = new_flag_manager(redis.connect(\"localhost\", 6379))\n```"},
    {"FlagManager.enabled", "fn enabled(self, flag_name, user_id)", "Checks if a feature flag is enabled for a user.", "```varian\nif fm.enabled(\"new_ui\", \"user_123\") { ... }\n```"},

    // event
    {"new_event_emitter", "fn new_event_emitter()", "Creates an in-memory event emitter.", "```varian\nlet ee = new_event_emitter()\n```"},
    {"EventEmitter.on", "fn on(self, event, handler)", "Registers an event listener handler function.", "```varian\nee.on(\"login\", |user| { print(user.name) })\n```"},
    {"EventEmitter.emit", "fn emit(self, event, payload)", "Emits an event triggering registered listeners.", "```varian\nee.emit(\"login\", { name: \"Alice\" })\n```"},

    // crypto
    {"crypto.constant_time_eq", "fn constant_time_eq(a, b)", "Compares two strings in constant time to prevent timing attacks.", "```varian\nlet valid = crypto.constant_time_eq(sig, expected)\n```"},
    {"crypto.hash_password", "fn hash_password(password)", "Hashes a password securely using bcrypt.", "```varian\nlet hash = crypto.hash_password(\"password123\")\n```"},
    {"crypto.verify_password", "fn verify_password(password, hash)", "Verifies a password against a hash.", "```varian\nlet ok = crypto.verify_password(\"pwd\", hash)\n```"},
    {"crypto.sha1_base64", "fn sha1_base64(str)", "Hashes a string using SHA-1 and returns Base64 representation.", "```varian\nlet b64 = crypto.sha1_base64(\"text\")\n```"},
    {"crypto.hash_sha256", "fn hash_sha256(str)", "Hashes a string using SHA-256.", "```varian\nlet hex = crypto.hash_sha256(\"text\")\n```"},
    {"crypto.sign_jwt", "fn sign_jwt(payload, secret)", "Signs a JWT payload with a HS256 secret.", "```varian\nlet token = crypto.sign_jwt({ sub: \"123\" }, \"secret\")\n```"},
    {"crypto.verify_jwt", "fn verify_jwt(token, secret)", "Verifies a JWT with a secret. Returns payload if valid, null otherwise.", "```varian\nlet claims = crypto.verify_jwt(token, \"secret\")\n```"},

    // csv
    {"csv.parse", "fn parse(csv_text)", "Parses CSV text into an array of arrays.", "```varian\nlet rows = csv.parse(\"a,b\\nc,d\")\n```"},
    {"csv.stringify", "fn stringify(rows)", "Serializes an array of arrays into CSV text.", "```varian\nlet csv_text = csv.stringify([[\"a\", \"b\"], [\"c\", \"d\"]])\n```"},

    // cache
    {"new_cache", "fn new_cache(conn)", "Creates a Redis-backed cache helper.", "```varian\nlet c = new_cache(redis.connect(\"localhost\", 6379))\n```"},
    {"Cache.get", "fn get(self, key)", "Retrieves a cached value from Redis.", "```varian\nlet val = c.get(\"users:1\")\n```"},
    {"Cache.set", "fn set(self, key, value, ttl_seconds)", "Sets a cache key to value with optional TTL.", "```varian\nc.set(\"users:1\", { name: \"Alice\" }, 3600)\n```"},
    {"Cache.delete", "fn delete(self, key)", "Deletes a cached value.", "```varian\nc.delete(\"users:1\")\n```"},
    {"Cache.remember", "fn remember(self, key, ttl_seconds, callback)", "Retrieves value or calls callback to set and return it.", "```varian\nlet user = c.remember(\"users:1\", 3600, || { return db.find(1) })\n```"},

    // config
    {"config.load", "fn load(path)", "Loads a JSON configuration file.", "```varian\nlet cfg = config.load(\"config.json\")\n```"},

    // auth
    {"new_auth", "fn new_auth(conn, secret)", "Creates a DB-backed session manager.", "```varian\nlet a = new_auth(sqlite.connect(\"db.sqlite\"), \"secret\")\n```"},
    {"Auth.create_session", "fn create_session(self, user_id)", "Creates a session for user_id and returns the token string.", "```varian\nlet token = a.create_session(user_id)\n```"},
    {"Auth.get_session", "fn get_session(self, token)", "Retrieves user_id from token if valid, null otherwise.", "```varian\nlet uid = a.get_session(token)\n```"},
    {"Auth.destroy_session", "fn destroy_session(self, token)", "Destroys a session.", "```varian\na.destroy_session(token)\n```"}
};

static const size_t native_docs_count = sizeof(native_docs) / sizeof(native_docs[0]);

#endif // NATIVE_DOCS_H
