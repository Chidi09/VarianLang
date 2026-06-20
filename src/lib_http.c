#include "lib_http.h"
#include "json.h"
#include "picohttpparser.h"
#include "lexer.h"
#include "parser.h"
#include <stdatomic.h>
#include <curl/curl.h>
#include <stdio.h>
#include <sys/uio.h>
#include <liburing.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <stdbool.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15 /* Linux value; not exposed under strict _POSIX_C_SOURCE */
#endif

/* ─── Dynamic buffer for collecting HTTP response ─── */
typedef struct { char *data; size_t len; } DynBuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    DynBuf *buf = (DynBuf *)userdata;
    char *new_data = (char *)realloc(buf->data, buf->len + total + 1);
    if (!new_data) return 0;
    memcpy(new_data + buf->len, ptr, total);
    buf->len += total;
    new_data[buf->len] = '\0';
    buf->data = new_data;
    return total;
}

/* The parser's method-name registry is global and untyped: any `impl`
 * method named e.g. "get" (Storage.get(), say) makes the parser emit a
 * dispatch call (module prepended as args[0]) for *any* later http.get(...)
 * too. Every http.* native checks for that defensively. */
static int http_arg_base(int arg_count, Value *args) {
    return (arg_count >= 1 && args[0].type == VAL_MODULE) ? 1 : 0;
}

/* ─── http.get(url) ─── */
static Value lib_http_get(VM *vm, int arg_count, Value *args) {
    int base = http_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING) return val_nil();
    const char *url = args[base].as.string->chars;
    CURL *curl = curl_easy_init();
    if (!curl) return val_nil();
    DynBuf buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Varian/0.1.0");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) { free(buf.data); return val_nil(); }
    if (!buf.data || buf.len == 0) { free(buf.data); return val_string(copy_string("", 0)); }
    ObjString *result = allocate_string(vm, buf.data, (int)buf.len);
    free(buf.data);
    return val_string(result);
}

/* ─── http.post(url, headers, body) ─── */
static Value lib_http_post(VM *vm, int arg_count, Value *args) {
    int base = http_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_STRING) {
        runtime_error(vm, "http.post(url, headers, body) requires url (string)");
        return val_nil();
    }
    const char *url = args[base].as.string->chars;
    Value headers_val = (arg_count > base + 1) ? args[base + 1] : val_nil();
    Value body_val = (arg_count > base + 2) ? args[base + 2] : val_nil();
    const char *body = (body_val.type == VAL_STRING) ? body_val.as.string->chars : "";

    CURL *curl = curl_easy_init();
    if (!curl) return val_nil();
    DynBuf buf = {NULL, 0};

    struct curl_slist *header_list = NULL;
    if (headers_val.type == VAL_ARRAY) {
        ObjArray *arr = headers_val.as.array;
        for (int i = 0; i < arr->count; i++) {
            if (arr->elements[i].type == VAL_STRING)
                header_list = curl_slist_append(header_list, arr->elements[i].as.string->chars);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Varian/0.1.0");
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (header_list) curl_slist_free_all(header_list);

    if (res != CURLE_OK) { free(buf.data); return val_nil(); }
    if (!buf.data || buf.len == 0) { free(buf.data); return val_string(copy_string("", 0)); }
    ObjString *result = allocate_string(vm, buf.data, (int)buf.len);
    free(buf.data);
    return val_string(result);
}

/* ═══════════════════════════════════════════
 *  HTTP Request Parsing
 * ═══════════════════════════════════════════ */

static const char *find_header_value(const char *buf, const char *header_name) {
    const char *headers = strstr(buf, "\r\n");
    if (!headers) return NULL;
    headers += 2;
    int name_len = (int)strlen(header_name);
    const char *h = headers;
    while (*h) {
        const char *eol = strstr(h, "\r\n");
        if (!eol) eol = h + strlen(h);
        if (eol == h) break;
        int cmp = strncasecmp(h, header_name, (size_t)name_len);
        if (cmp == 0 && h[name_len] == ':') {
            const char *val = h + name_len + 1;
            while (*val == ' ') val++;
            return val;
        }
        h = eol + 2;
    }
    return NULL;
}

/* ═══════════════════════════════════════════
 *  Connection buffering, request framing limits, keep-alive
 *
 *  Each accepted connection gets its own growable ConnBuffer instead of one
 *  fixed 64KB stack buffer per request: that fixed buffer used to (a) just
 *  stop reading and silently hand a truncated body to the handler for any
 *  request bigger than 64KB, leaving the unread remainder sitting on the
 *  socket to corrupt the next request's framing on the same connection, and
 *  (b) have no real size limit at all -- a client claiming a 2GB
 *  Content-Length would have a 64KB buffer accepted, truncated, and
 *  processed as if nothing were wrong. MAX_REQUEST_HEADER_SIZE/
 *  MAX_REQUEST_BODY_SIZE below are enforced explicitly, with a clean
 *  431/413 response, instead.
 * ═══════════════════════════════════════════ */

#define MAX_REQUEST_HEADER_SIZE (32 * 1024)        /* matches common nginx/Apache defaults */
#define MAX_REQUEST_BODY_SIZE   (10 * 1024 * 1024) /* 10MB; large uploads need a streaming API this doesn't have yet */
#define CONN_IDLE_TIMEOUT_SEC   30.0   /* close a connection (new or kept-alive) idle this long -- this IS the slowloris defense: a client that connects and trickles data (or never sends any) ties up nothing but its own ConnBuffer, checked with a non-blocking recv() each tick, never a blocking read */
#define CONN_MAX_REQUESTS       1000   /* cap requests per kept-alive connection (nginx's keepalive_requests default) */
#define MAX_PENDING_CONNS       1024   /* hard cap on concurrently tracked connections per worker */

typedef struct {
    char *data;
    int len;
    int capacity;
} ConnBuffer;

static void conn_buffer_init(ConnBuffer *cb) {
    cb->capacity = 8192;
    cb->data = (char *)malloc((size_t)cb->capacity);
    cb->len = 0;
    if (cb->data) cb->data[0] = '\0';
}

static void conn_buffer_free(ConnBuffer *cb) {
    free(cb->data);
    cb->data = NULL;
    cb->len = 0;
    cb->capacity = 0;
}

/* Drops the first n bytes (a request just fully processed), shifting any
 * leftover bytes -- the start of a pipelined next request -- to the front
 * so the next poll can find it without another recv(). */
static void conn_buffer_consume(ConnBuffer *cb, int n) {
    if (n >= cb->len) {
        cb->len = 0;
        if (cb->data) cb->data[0] = '\0';
        return;
    }
    memmove(cb->data, cb->data + n, (size_t)(cb->len - n));
    cb->len -= n;
    cb->data[cb->len] = '\0';
}

/* Plain-socket vs TLS recv, behind one interface. Same return contract as
 * conn_buffer_recv_more below: >0 bytes, 0 orderly close, -1 try again
 * later (EAGAIN, or TLS's WANT_READ/WANT_WRITE -- a TLS read can need to
 * *write* first, e.g. a renegotiation, so WANT_WRITE is not an error
 * here), -2 a real error. */
static int conn_io_recv(int fd, SSL *ssl, char *buf, int want) {
    if (ssl) {
        int r = SSL_read(ssl, buf, want);
        if (r > 0) return r;
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return -1;
        if (err == SSL_ERROR_ZERO_RETURN) return 0;
        return -2;
    }
    int r = (int)recv(fd, buf, (size_t)want, 0);
    if (r > 0) return r;
    if (r == 0) return 0;
    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK) return -1;
    return -2;
}

/* One non-blocking read into the buffer, growing capacity as needed (up
 * to max_total + 1 -- given the header/body limits are checked in
 * try_frame_request before this is ever asked to grow past them, a
 * legitimate request should never actually hit that ceiling).
 * Returns: >0 = bytes read, 0 = orderly close, -1 = no data right now
 * (try again next tick), -2 = a real error (or, defensively, the capacity
 * ceiling above, which shouldn't happen) -- the caller should close the
 * connection. ssl is NULL for a plain HTTP connection. */
static int conn_buffer_recv_more(int fd, SSL *ssl, ConnBuffer *cb, int max_total) {
    if (cb->len + 1 >= cb->capacity) {
        int new_cap = cb->capacity * 2;
        int cap_limit = max_total + 1;
        if (new_cap > cap_limit) new_cap = cap_limit;
        if (new_cap <= cb->capacity) return -2;
        char *new_data = (char *)realloc(cb->data, (size_t)new_cap);
        if (!new_data) return -2;
        cb->data = new_data;
        cb->capacity = new_cap;
    }
    int want = cb->capacity - cb->len - 1;
    if (want <= 0) return -2;
    int r = conn_io_recv(fd, ssl, cb->data + cb->len, want);
    if (r > 0) {
        cb->len += r;
        cb->data[cb->len] = '\0';
    }
    return r;
}

/* Best-effort send over a plain socket or TLS, retrying on partial writes.
 * A handful of WANT_READ/WANT_WRITE retries for TLS (e.g. a renegotiation
 * mid-write) are looped through directly rather than deferred to a later
 * scheduler tick -- response bodies are typically small enough that this
 * resolves in a handful of iterations, and the alternative (threading
 * response-sending through the same multi-tick state machine as the
 * connection read path) is a lot of added complexity for a case that, in
 * practice, essentially never blocks for long. */
static void conn_io_send_all(int fd, SSL *ssl, const char *buf, int len) {
    int sent = 0;
    int stall_guard = 0;
    while (sent < len && stall_guard < 100000) {
        int n;
        if (ssl) {
            n = SSL_write(ssl, buf + sent, len - sent);
            if (n <= 0) {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    stall_guard++;
                    continue;
                }
                return;
            }
        } else {
            n = (int)send(fd, buf + sent, (size_t)(len - sent), 0);
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    stall_guard++;
                    continue;
                }
                return;
            }
        }
        sent += n;
    }
}

typedef enum {
    REQ_INCOMPLETE,
    REQ_READY,
    REQ_HEADERS_TOO_LARGE,
    REQ_BODY_TOO_LARGE,
} ReqFrameStatus;

/* Checks whether cb currently holds one complete request (full headers +
 * full declared body, if any). Never reads from the socket -- pure framing
 * logic over whatever bytes are already buffered. */
static ReqFrameStatus try_frame_request(ConnBuffer *cb, int *out_total_len) {
    char *header_end = NULL;
    for (int i = 0; i + 4 <= cb->len; i++) {
        if (memcmp(cb->data + i, "\r\n\r\n", 4) == 0) { header_end = cb->data + i + 4; break; }
    }
    if (!header_end) {
        if (cb->len >= MAX_REQUEST_HEADER_SIZE) return REQ_HEADERS_TOO_LARGE;
        return REQ_INCOMPLETE;
    }
    int header_len = (int)(header_end - cb->data);
    /* The terminator can arrive already past the limit if it shows up in
     * the same recv() as everything before it (a single oversized chunk),
     * not just when it's still missing -- check unconditionally. */
    if (header_len > MAX_REQUEST_HEADER_SIZE) return REQ_HEADERS_TOO_LARGE;
    const char *cl = find_header_value(cb->data, "Content-Length");
    int content_length = cl ? atoi(cl) : 0;
    if (content_length < 0) content_length = 0;
    if (content_length > MAX_REQUEST_BODY_SIZE) return REQ_BODY_TOO_LARGE;
    int total_len = header_len + content_length;
    if (cb->len < total_len) return REQ_INCOMPLETE;
    *out_total_len = total_len;
    return REQ_READY;
}

/* ═══════════════════════════════════════════
 *  Struct Creation Helpers
 * ═══════════════════════════════════════════ */

static Value make_request(VM *vm, const char *method, const char *path,
                           Value body_val, Value json_val, Value params_val,
                           Value headers_val, const char *ip_str, int client_fd) {
    int total = 8;
    ObjStruct *req = new_struct(vm, total, false);

    req->field_names[0] = strdup("method");
    req->fields[0] = val_string(allocate_string(vm, method, (int)strlen(method)));

    req->field_names[1] = strdup("path");
    req->fields[1] = val_string(allocate_string(vm, path, (int)strlen(path)));

    req->field_names[2] = strdup("body");
    req->fields[2] = (body_val.type == VAL_STRING && body_val.as.string->length > 0) ? body_val : val_nil();

    req->field_names[3] = strdup("json");
    req->fields[3] = json_val;

    req->field_names[4] = strdup("params");
    req->fields[4] = params_val;

    req->field_names[5] = strdup("headers");
    req->fields[5] = headers_val;

    req->field_names[6] = strdup("ip");
    req->fields[6] = val_string(allocate_string(vm, ip_str, (int)strlen(ip_str)));

    req->field_names[7] = strdup("socket_fd");
    req->fields[7] = val_int(client_fd);

    return val_struct(req);
}

static void free_request(VM *vm, Value req_val) {
    (void)vm;
    (void)req_val;
    // Let the VM Garbage Collector handle the lifecycle of this struct
    // to prevent double frees, dangling pointer references, and segmentation faults.
}

/* ═══════════════════════════════════════════
 *  Route Matching
 * ═══════════════════════════════════════════ */

#define MAX_PARAMS 16

static bool match_route(const char *pattern, const char *path,
                         char *param_names[], int param_name_lens[],
                         int param_starts[], int param_lens[],
                         int *param_count) {
    *param_count = 0;
    const char *pp = pattern;
    const char *qp = path;
    while (*pp && *qp) {
        while (*pp == '/') pp++;
        while (*qp == '/') qp++;
        if (!*pp && !*qp) break;
        if (!*pp || !*qp) return false;
        const char *pe = pp;
        while (*pe && *pe != '/') pe++;
        const char *qe = qp;
        while (*qe && *qe != '/') qe++;
        int plen = (int)(pe - pp);
        int qlen = (int)(qe - qp);
        if (*pp == ':') {
            if (*param_count >= MAX_PARAMS) return false;
            param_names[*param_count] = (char *)(pp + 1);
            param_name_lens[*param_count] = plen - 1;
            param_starts[*param_count] = (int)(qp - path);
            param_lens[*param_count] = qlen;
            (*param_count)++;
        } else {
            if (plen != qlen || strncmp(pp, qp, (size_t)plen) != 0) return false;
        }
        pp = pe;
        qp = qe;
    }
    while (*pp == '/') pp++;
    while (*qp == '/') qp++;
    return *pp == '\0' && *qp == '\0';
}

/* ═══════════════════════════════════════════
 *  Response Sending
 * ═══════════════════════════════════════════ */

static void send_http_response(int client_fd, SSL *ssl, Value result, bool keep_alive) {
    int status = 200;
    const char *body = NULL;
    int body_len = 0;
    const char *content_type = "text/plain";
    ObjStruct *headers_struct = NULL;
    if (result.type == VAL_STRING) {
        body = result.as.string->chars;
        body_len = result.as.string->length;
    } else if (result.type == VAL_STRUCT) {
        ObjStruct *rs = result.as.structure;
        for (int i = 0; i < rs->field_count; i++) {
            if (strcmp(rs->field_names[i], "status") == 0 && rs->fields[i].type == VAL_INT)
                status = (int)rs->fields[i].as.integer;
            if (strcmp(rs->field_names[i], "body") == 0 && rs->fields[i].type == VAL_STRING) {
                body = rs->fields[i].as.string->chars;
                body_len = rs->fields[i].as.string->length;
            }
            if (strcmp(rs->field_names[i], "content_type") == 0 && rs->fields[i].type == VAL_STRING)
                content_type = rs->fields[i].as.string->chars;
            if (strcmp(rs->field_names[i], "headers") == 0 && rs->fields[i].type == VAL_STRUCT)
                headers_struct = rs->fields[i].as.structure;
        }
    }
    if (!body) { body = ""; body_len = 0; }
    const char *status_desc = "OK";
    if (status == 404) status_desc = "Not Found";
    else if (status == 500) status_desc = "Internal Server Error";
    else if (status == 302) status_desc = "Found";
    else if (status == 301) status_desc = "Moved Permanently";
    else if (status == 400) status_desc = "Bad Request";
    else if (status == 403) status_desc = "Forbidden";
    else if (status == 204) status_desc = "No Content";
    else if (status == 429) status_desc = "Too Many Requests";

    char headers_buf[8192] = "";
    int headers_len = 0;
    if (headers_struct) {
        for (int i = 0; i < headers_struct->field_count; i++) {
            if (headers_struct->fields[i].type == VAL_STRING) {
                char header_name[256];
                snprintf(header_name, sizeof(header_name), "%s", headers_struct->field_names[i]);
                for (int j = 0; header_name[j]; j++) {
                    if (header_name[j] == '_') header_name[j] = '-';
                }
                headers_len += snprintf(headers_buf + headers_len, sizeof(headers_buf) - headers_len,
                    "%s: %s\r\n", header_name, headers_struct->fields[i].as.string->chars);
            }
        }
    }

    /* Phase 1.2: scatter/gather I/O with writev — send the status line,
     * Content-Type/Content-Length headers, custom headers, and body as
     * separate iovec entries in a single syscall. No TLS path since
     * SSL_write doesn't support writev (we fall back to the old two-send
     * approach for SSL). */
    char resp_buf[16384];
    int hn = snprintf(resp_buf, sizeof(resp_buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: %s\r\n"
        "%s"
        "Connection: %s\r\n"
        "\r\n",
        status, status_desc, body_len, content_type, headers_buf,
        keep_alive ? "keep-alive" : "close");
    if (hn <= 0) return;
    if (hn >= (int)sizeof(resp_buf)) hn = (int)sizeof(resp_buf) - 1;

    if (ssl) {
        conn_io_send_all(client_fd, ssl, resp_buf, hn);
        if (body_len > 0) conn_io_send_all(client_fd, ssl, body, body_len);
    } else {
        struct iovec iov[2];
        int iovcnt = 0;
        iov[iovcnt].iov_base = resp_buf;
        iov[iovcnt].iov_len = (size_t)hn;
        iovcnt++;
        if (body_len > 0) {
            iov[iovcnt].iov_base = (void *)(uintptr_t)body;
            iov[iovcnt].iov_len = (size_t)body_len;
            iovcnt++;
        }
        int stall_guard = 0;
        size_t total = (size_t)hn + (size_t)body_len;
        size_t sent = 0;
        while (sent < total && stall_guard < 100000) {
            struct iovec cur_iov[2];
            int cur_cnt = 0;
            size_t skip = sent;
            for (int j = 0; j < iovcnt; j++) {
                if (skip < iov[j].iov_len) {
                    cur_iov[cur_cnt].iov_base = (char *)iov[j].iov_base + skip;
                    cur_iov[cur_cnt].iov_len = iov[j].iov_len - skip;
                    cur_cnt++;
                    skip = 0;
                } else {
                    skip -= iov[j].iov_len;
                }
            }
            if (cur_cnt == 0) break;
            int n = (int)writev(client_fd, cur_iov, cur_cnt);
            if (n > 0) sent += (size_t)n;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) { stall_guard++; }
            else break;
        }
    }
}

/* ═══════════════════════════════════════════
 *  Handler Executor
 * ═══════════════════════════════════════════ */

static bool response_wants_keep_open(Value result) {
    if (result.type != VAL_STRUCT) return false;
    ObjStruct *rs = result.as.structure;
    for (int i = 0; i < rs->field_count; i++) {
        if (strcmp(rs->field_names[i], "_keep_open") == 0 &&
            rs->fields[i].type == VAL_BOOL && rs->fields[i].as.boolean) {
            return true;
        }
    }
    return false;
}

/* Most handlers finish in a single tick and call_handler() can just return
 * the result synchronously. A handler managing a long-lived connection
 * (WebSocket/SSE) calls task.yield() while waiting for more socket data --
 * that must NOT be torn down as "done" the moment it yields once, the same
 * way a task.spawn()'d task or an actor loop keeps running across many
 * scheduler ticks. When that happens this registers the task with the
 * normal round-robin scheduler (vm_register_task) and sets *deferred so the
 * caller leaves the client fd alone; http_finalize_deferred_response()
 * (called from the scheduler in vm.c once the task actually finishes) sends
 * the real response and closes the fd at that point instead. */
static Value call_handler(VM *vm, Value handler_val, Value req_val, int client_fd, SSL *ssl, bool *deferred) {
    *deferred = false;
    ObjClosure *handler_closure = (handler_val.type == VAL_CLOSURE) ? handler_val.as.closure : NULL;
    ObjFunction *handler_fn = handler_closure ? handler_closure->function : handler_val.as.function;
    Task *tmp = task_new(vm);
    task_arena_enable(tmp);
    /* Register *before* running: gc_mark_roots() only scans stacks of tasks
     * in vm->tasks, so if tmp isn't registered yet, anything the handler
     * allocates (including req_val's own fields, still live on tmp's stack)
     * is invisible to the GC -- a GC cycle triggered by the handler itself
     * (e.g. a native call that allocates a string) can then free memory
     * that's still in active use. This was a real, reproduced heap-use-
     * after-free under load, not a theoretical concern. */
    vm_register_task(vm, tmp);
    tmp->stack[tmp->stack_top++] = req_val;
    tmp->frames[0].function = handler_fn;
    tmp->frames[0].closure = handler_closure;
    tmp->frames[0].ip = handler_fn->code;
    tmp->frames[0].slots = tmp->stack;
    tmp->frames[0].return_base = 0;
    tmp->frame_count = 1;

    Task *prev = vm->current_task;
    vm->current_task = tmp;
    bool ok = task_run(vm, tmp);
    vm->current_task = prev;

    if (!ok || vm->had_error) {
        /* The handler threw without a try/catch (or hit some other fatal
         * VM error) -- this must turn into a 500 for *this* request, not
         * bring down the whole server. vm->had_error has to be cleared
         * here: it's still checked by the round-robin scheduler once
         * control returns up through handle_connection() -> lib_http_serve()
         * to whichever task is actually running http.serve() (the main
         * script task in the common case) -- leaving it set would get that
         * task killed too, taking every other connection (and, without
         * clustering, the whole process) down with it. */
        fprintf(stderr, "Unhandled error in request handler: %s\n", vm->last_error);
        vm->had_error = false;
        /* Leave tmp registered (just dead) rather than freeing it here --
         * the round-robin scheduler reaps dead tasks once a full pass over
         * vm->tasks completes (see vm_run in vm.c), by which point
         * handle_connection() is guaranteed to be done using whatever this
         * function returns. Freeing it here instead would race against
         * that: this Task's stack is the only GC root keeping the request
         * struct's fields alive, and the result struct built below could
         * still be in use when the next GC cycle runs. */
        tmp->dead = true;
        ObjStruct *rs = new_struct(vm, 2, false);
        rs->field_names[0] = strdup("status");
        rs->fields[0] = val_int(500);
        rs->field_names[1] = strdup("body");
        rs->fields[1] = val_string(allocate_string(vm, "Internal Server Error", 22));
        return val_struct(rs);
    }

    if (!tmp->dead) {
        tmp->http_response_fd = client_fd;
        tmp->http_response_ssl = (void *)ssl;
        *deferred = true;
        return val_nil();
    }

    Value result = val_nil();
    if (tmp->stack_top > 0)
        result = tmp->stack[tmp->stack_top - 1];
    /* tmp stays registered (just dead) until the round-robin scheduler's
     * end-of-pass reap, the same reasoning as the error path above: result
     * is still only reachable via tmp's stack until handle_connection()
     * finishes using it. */
    tmp->dead = true;
    return result;
}

/* Called from the VM's round-robin scheduler (vm.c) once a deferred handler
 * task (see call_handler above) actually finishes -- normally, or via an
 * uncaught error partway through a long-lived connection (e.g. a bad
 * WebSocket message). Either way this must not propagate further: finalize
 * this one connection and let everything else keep running. */
void http_finalize_deferred_response(VM *vm, Task *t, bool had_error) {
    int fd = t->http_response_fd;
    SSL *ssl = (SSL *)t->http_response_ssl;
    t->http_response_fd = -1;
    t->http_response_ssl = NULL;
    if (had_error) {
        fprintf(stderr, "Unhandled error in long-lived request handler\n");
        ObjStruct *rs = new_struct(vm, 2, false);
        rs->field_names[0] = strdup("status");
        rs->fields[0] = val_int(500);
        rs->field_names[1] = strdup("body");
        rs->fields[1] = val_string(copy_string("Internal Server Error", 22));
        send_http_response(fd, ssl, val_struct(rs), false);
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        close(fd);
        return;
    }
    Value result = val_nil();
    if (t->stack_top > 0)
        result = t->stack[t->stack_top - 1];
    if (!response_wants_keep_open(result)) {
        send_http_response(fd, ssl, result, false);
    }
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    close(fd);
}

/* ═══════════════════════════════════════════
 *  Phase 3: Radix Trie for Native Route Matching
 *
 *  O(path_length) lookup instead of O(route_count) linear scan. Each node
 *  stores a path segment, child pointers, and an optional handler Value.
 *  Parameter segments (":name") capture a variable part of the path.
 * ═══════════════════════════════════════════ */

#define RADIX_MAX_PARAMS 16
#define RADIX_MAX_CHILDREN 32

typedef struct RadixNode {
    char *segment;
    int segment_len;
    struct RadixNode *children[RADIX_MAX_CHILDREN];
    int child_count;
    Value handler;
    bool is_param;
    char *param_name;
    int param_namelen;
} RadixNode;

static RadixNode *radix_node_new(const char *seg, int seg_len) {
    RadixNode *n = (RadixNode *)calloc(1, sizeof(RadixNode));
    if (!n) return NULL;
    n->segment = (char *)malloc(seg_len + 1);
    memcpy(n->segment, seg, seg_len);
    n->segment[seg_len] = '\0';
    n->segment_len = seg_len;
    n->handler = val_nil();
    n->is_param = (seg_len > 0 && seg[0] == ':');
    if (n->is_param) {
        n->param_name = (char *)malloc(seg_len);
        memcpy(n->param_name, seg + 1, seg_len - 1);
        n->param_namelen = seg_len - 1;
        n->param_name[seg_len - 1] = '\0';
    }
    return n;
}

static void radix_node_free(RadixNode *n) {
    if (!n) return;
    for (int i = 0; i < n->child_count; i++)
        radix_node_free(n->children[i]);
    free(n->segment);
    free(n->param_name);
    free(n);
}

static RadixNode *radix_insert(RadixNode *root, const char *method, const char *path, Value handler) {
    RadixNode *cur = root;
    bool found = false;
    for (int i = 0; i < cur->child_count; i++) {
        if (strcmp(cur->children[i]->segment, method) == 0) {
            cur = cur->children[i];
            found = true;
            break;
        }
    }
    if (!found) {
        RadixNode *mn = radix_node_new(method, (int)strlen(method));
        if (cur->child_count < RADIX_MAX_CHILDREN)
            cur->children[cur->child_count++] = mn;
        cur = mn;
    }
    int i = 0;
    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;
        int start = i;
        while (path[i] && path[i] != '/') i++;
        int seg_len = i - start;
        found = false;
        for (int j = 0; j < cur->child_count; j++) {
            RadixNode *child = cur->children[j];
            if (child->segment_len == seg_len &&
                memcmp(child->segment, path + start, seg_len) == 0) {
                cur = child;
                found = true;
                break;
            }
        }
        if (!found) {
            RadixNode *nn = radix_node_new(path + start, seg_len);
            if (cur->child_count < RADIX_MAX_CHILDREN)
                cur->children[cur->child_count++] = nn;
            cur = nn;
        }
    }
    cur->handler = handler;
    return cur;
}

typedef struct {
    char name[64];
    char value[1024];
} RadixParam;

static Value radix_lookup(RadixNode *root, const char *method, const char *path,
                           RadixParam *params, int *param_count) {
    *param_count = 0;
    if (!root) return val_nil();
    RadixNode *cur = NULL;
    for (int i = 0; i < root->child_count; i++) {
        if (strcmp(root->children[i]->segment, method) == 0) {
            cur = root->children[i];
            break;
        }
    }
    if (!cur) return val_nil();
    int pos = 0;
    while (cur && path[pos]) {
        while (path[pos] == '/') pos++;
        if (!path[pos]) break;
        int start = pos;
        while (path[pos] && path[pos] != '/') pos++;
        int seg_len = pos - start;
        RadixNode *matched = NULL;
        for (int i = 0; i < cur->child_count; i++) {
            RadixNode *child = cur->children[i];
            if (child->is_param) {
                matched = child;
                if (*param_count < RADIX_MAX_PARAMS) {
                    int clen = seg_len < (int)sizeof(params[*param_count].value) - 1 ? seg_len : (int)sizeof(params[*param_count].value) - 1;
                    memcpy(params[*param_count].value, path + start, clen);
                    params[*param_count].value[clen] = '\0';
                    memcpy(params[*param_count].name, child->param_name, child->param_namelen);
                    params[*param_count].name[child->param_namelen] = '\0';
                    (*param_count)++;
                }
                break;
            } else if (child->segment_len == seg_len &&
                       memcmp(child->segment, path + start, seg_len) == 0) {
                matched = child;
                break;
            }
        }
        cur = matched;
    }
    if (cur && cur->handler.type != VAL_NIL) return cur->handler;
    if (cur && cur->is_param && cur->handler.type != VAL_NIL) return cur->handler;
    return val_nil();
}

/* ═══════════════════════════════════════════
 *  Handle a single connection (shared by serve and serve_with_routes)
 *  If routes_val is not nil, it's a VAL_ARRAY of route structs to match
 * ═══════════════════════════════════════════ */

/* request_count is this connection's 1-based count of requests served so
 * far (including this one) -- used to cap keep-alive lifetime regardless of
 * what the client asks for (CONN_MAX_REQUESTS). buf/buf_len are exactly one
 * already-framed request (see try_frame_request) -- this function no
 * longer touches the socket for reading at all, only (maybe) for writing
 * the response.
 *
 * Returns whether the connection should stay open for another keep-alive
 * request. *out_transferred is set true if the handler took ownership of
 * the raw fd itself (a WebSocket/SSE upgrade, deferred or not -- see
 * call_handler/response_wants_keep_open) -- the caller must not close it,
 * read from it, or otherwise manage it as an HTTP connection again. */
#define PHR_MAX_HEADERS 100

static const char *find_phr_header_value(const struct phr_header *headers, size_t num_headers,
                                          const char *name, size_t name_len) {
    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == name_len &&
            strncasecmp(headers[i].name, name, name_len) == 0) {
            return headers[i].value;
        }
    }
    return NULL;
}

static Value parse_all_headers_phr(VM *vm, const struct phr_header *phr_headers, size_t num_headers) {
    if (num_headers == 0) return val_nil();
    ObjStruct *hs = new_struct(vm, (int)num_headers, false);
    Task *self = vm->current_task;
    self->stack[self->stack_top++] = val_struct(hs);
    int idx = 0;
    for (size_t i = 0; i < num_headers; i++) {
        if (phr_headers[i].name_len == 0) continue;
        char *name = (char *)malloc(phr_headers[i].name_len + 1);
        if (!name) continue;
        memcpy(name, phr_headers[i].name, phr_headers[i].name_len);
        name[phr_headers[i].name_len] = '\0';
        for (size_t j = 0; j < phr_headers[i].name_len; j++) {
            if (name[j] == '-') name[j] = '_';
            name[j] = (char)tolower((unsigned char)name[j]);
        }
        hs->field_names[idx] = name;
        int val_len = (int)phr_headers[i].value_len;
        hs->fields[idx] = val_string(allocate_string(vm, phr_headers[i].value, val_len));
        idx++;
    }
    self->stack_top--;
    return val_struct(hs);
}

static bool determine_keep_alive_phr(int request_count, int minor_version,
                                      const struct phr_header *headers, size_t num_headers) {
    if (request_count >= CONN_MAX_REQUESTS) return false;
    bool is_http_11 = (minor_version == 1);
    const char *conn = find_phr_header_value(headers, num_headers, "Connection", 10);
    if (conn) {
        if (strncasecmp(conn, "close", 5) == 0) return false;
        if (strncasecmp(conn, "keep-alive", 10) == 0) return true;
    }
    return is_http_11;
}

static bool handle_connection(VM *vm, int client_fd, SSL *ssl, const char *ip_str,
                               const char *buf, int buf_len, int request_count,
                               Value handler_or_routes_val, bool is_routed,
                               RadixNode *route_trie, bool *out_transferred) {
    *out_transferred = false;

    /* Phase 1.1: single-pass SIMD parsing with picohttpparser */
    const char *method_ptr, *path_ptr;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header phr_headers[PHR_MAX_HEADERS];
    size_t num_phr_headers = PHR_MAX_HEADERS;
    int consumed = phr_parse_request(buf, (size_t)buf_len,
                                      &method_ptr, &method_len,
                                      &path_ptr, &path_len,
                                      &minor_version,
                                      phr_headers, &num_phr_headers, 0);
    if (consumed < 0) {
        ObjStruct *rs = new_struct(vm, 2, false);
        rs->field_names[0] = strdup("status");
        rs->fields[0] = val_int(400);
        rs->field_names[1] = strdup("body");
        rs->fields[1] = val_string(copy_string("Bad Request", 11));
        send_http_response(client_fd, ssl, val_struct(rs), false);
                        return false;
                    }
    char method[64], path[2048];
    if (method_len >= sizeof(method)) method_len = sizeof(method) - 1;
    memcpy(method, method_ptr, method_len); method[method_len] = '\0';
    if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
    memcpy(path, path_ptr, path_len); path[path_len] = '\0';

    bool keep_alive = determine_keep_alive_phr(request_count, minor_version, phr_headers, num_phr_headers);
    Value headers_val = parse_all_headers_phr(vm, phr_headers, num_phr_headers);

    const char *body_str = (consumed < buf_len) ? buf + consumed : NULL;
    int body_len = body_str ? buf_len - consumed : 0;
    Value body_val = val_nil();
    if (body_len > 0)
        body_val = val_string(allocate_string(vm, body_str, body_len));

    Value json_val = val_nil();
    const char *ct = find_phr_header_value(phr_headers, num_phr_headers, "Content-Type", 12);
    if (ct && body_len > 0 && strstr(ct, "application/json")) {
        char *json_buf = (char *)malloc((size_t)body_len + 1);
        memcpy(json_buf, body_str, (size_t)body_len);
        json_buf[body_len] = '\0';
        json_val = json_decode(vm, json_buf);
        free(json_buf);
    }
    Value result = val_nil();
    bool deferred = false;
    if (is_routed) {
        /* Phase 3: radix trie lookup — O(path_length) instead of O(routes) */
        bool found = false;
        Value route_handler = val_nil();
        RadixParam params_flat[RADIX_MAX_PARAMS];
        int param_count = 0;

        if (route_trie) {
            route_handler = radix_lookup(route_trie, method, path, params_flat, &param_count);
            found = (route_handler.type != VAL_NIL);
        }

        if (!found && route_trie == NULL) {
            /* Fallback: linear scan (no trie was built, e.g. routes passed by value) */
            Value routes_val = handler_or_routes_val;
            if (routes_val.type == VAL_ARRAY) {
                ObjArray *routes = routes_val.as.array;
                for (int i = 0; i < routes->count; i++) {
                    if (routes->elements[i].type != VAL_STRUCT) continue;
                    ObjStruct *route = routes->elements[i].as.structure;
                    const char *route_method = NULL;
                    const char *route_path = NULL;
                    route_handler = val_nil();
                    for (int j = 0; j < route->field_count; j++) {
                        if (strcmp(route->field_names[j], "method") == 0 && route->fields[j].type == VAL_STRING)
                            route_method = route->fields[j].as.string->chars;
                        if (strcmp(route->field_names[j], "path") == 0 && route->fields[j].type == VAL_STRING)
                            route_path = route->fields[j].as.string->chars;
                        if (strcmp(route->field_names[j], "handler") == 0 &&
                            (route->fields[j].type == VAL_FUNCTION || route->fields[j].type == VAL_CLOSURE))
                            route_handler = route->fields[j];
                    }
                    if (!route_method || !route_path || route_handler.type == VAL_NIL) continue;
                    if (strcasecmp(route_method, method) != 0) continue;
                    char *param_names[MAX_PARAMS];
                    int param_name_lens[MAX_PARAMS];
                    int param_starts[MAX_PARAMS];
                    int param_lens[MAX_PARAMS];
                    param_count = 0;
                    if (match_route(route_path, path, param_names, param_name_lens,
                                     param_starts, param_lens, &param_count)) {
                        found = true;
                        /* Convert old-style params to flat RadixParam */
                        for (int p = 0; p < param_count && p < RADIX_MAX_PARAMS; p++) {
                            int nl = param_name_lens[p] < 63 ? param_name_lens[p] : 63;
                            memcpy(params_flat[p].name, param_names[p], nl);
                            params_flat[p].name[nl] = '\0';
                            int vl = param_lens[p] < 1023 ? param_lens[p] : 1023;
                            memcpy(params_flat[p].value, path + param_starts[p], vl);
                            params_flat[p].value[vl] = '\0';
                        }
                        break;
                    }
                }
            }
        }

        if (found) {
            Value params_val = val_nil();
            if (param_count > 0) {
                ObjStruct *ps = (ObjStruct *)calloc(1, sizeof(ObjStruct));
                ps->obj.type = VAL_STRUCT;
                ps->obj.next = vm->objects;
                vm->objects = (Obj *)ps;
                ps->field_count = param_count;
                ps->field_names = (char **)calloc((size_t)param_count, sizeof(char *));
                ps->fields = (Value *)calloc((size_t)param_count, sizeof(Value));
                ps->type_name = NULL;
                for (int p = 0; p < param_count; p++) {
                    ps->field_names[p] = strdup(params_flat[p].name);
                    ps->fields[p] = val_string(allocate_string(vm, params_flat[p].value, (int)strlen(params_flat[p].value)));
                }
                params_val = val_struct(ps);
            }
            Value req_val = make_request(vm, method, path, body_val, json_val, params_val, headers_val, ip_str, client_fd);
            result = call_handler(vm, route_handler, req_val, client_fd, ssl, &deferred);
            free_request(vm, req_val);
        } else {
            result = val_nil();
            ObjStruct *rs = new_struct(vm, 2, false);
            rs->field_names[0] = strdup("status");
            rs->fields[0] = val_int(404);
            rs->field_names[1] = strdup("body");
            rs->fields[1] = val_string(allocate_string(vm, "Not Found", 9));
            Value not_found = val_struct(rs);
            send_http_response(client_fd, ssl, not_found, keep_alive);
            free_request(vm, not_found);
            return keep_alive;
        }
    } else {
        Value req_val = make_request(vm, method, path, body_val, json_val, val_nil(), headers_val, ip_str, client_fd);
        result = call_handler(vm, handler_or_routes_val, req_val, client_fd, ssl, &deferred);
        free_request(vm, req_val);
    }

    /* The handler yielded instead of completing (a long-lived WebSocket/SSE
     * connection waiting on more data) -- it's now a real scheduled task
     * that owns this fd's fate; http_finalize_deferred_response() sends the
     * real response and closes it once that task actually finishes. Don't
     * touch `result` (it's nil, the task hasn't produced one yet) or the fd
     * here. */
    if (deferred) {
        *out_transferred = true;
        return false;
    }

    if (response_wants_keep_open(result)) {
        *out_transferred = true;
        return false;
    }
    send_http_response(client_fd, ssl, result, keep_alive);
    return keep_alive;
}





/* Cluster worker forward declaration */
static void spawn_cluster_workers(int worker_count, int port);

/* ═══════════════════════════════════════════
 *  Pending connection pool -- non-blocking multiplexing
 *
 *  One ConnBuffer-backed slot per accepted-but-not-yet-fully-read connection
 *  AND per kept-alive connection waiting for its next request. Every fd in
 *  here is non-blocking and polled with a single non-blocking recv() per
 *  tick -- a slow, idle, or malicious (slowloris-style) client only ever
 *  costs one cheap EAGAIN recv() per tick, never a blocking read, and never
 *  delays any other connection. This is what makes the per-fd
 *  CONN_IDLE_TIMEOUT_SEC check the actual slowloris defense (see its
 *  definition above) rather than just documentation.
 * ═══════════════════════════════════════════ */

typedef struct {
    int fd;
    char ip[64];
    ConnBuffer buf;
    double last_activity;
    int request_count;
    SSL *ssl;             /* NULL for a plain HTTP connection */
    bool tls_handshaking; /* true until SSL_accept() completes */
    bool io_uring_pending; /* outstanding io_uring recv SQE for this fd */
    int io_uring_recv_off; /* buf.len at the moment the outstanding SQE was
                             * submitted -- the absolute offset the kernel
                             * will actually write to. conn_buffer_consume()
                             * can shrink buf.len (via memmove) while that
                             * SQE is still in flight, so on completion we
                             * cannot assume the kernel wrote at the
                             * *current* buf.len; we must reconcile against
                             * this saved offset instead. */
} PendingConn;

typedef struct {
    PendingConn *conns;
    int count;
    int capacity;
    int epoll_fd;          /* epoll instance fd for TLS; -1 if not initialized */
    SSL_CTX *tls_ctx;      /* NULL for a plain (non-TLS) listener */
    RadixNode *route_trie; /* Phase 3: radix trie for fast route matching */
    /* Phase 4: io_uring for zero-syscall I/O on plain HTTP connections */
    struct io_uring ring;
    bool use_io_uring;
    int listen_fd;         /* for submitting accept SQEs */
} PendingConnPool;

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static PendingConnPool *get_pending_pool(Task *t) {
    if (!t->http_pending_conns) {
        t->http_pending_conns = calloc(1, sizeof(PendingConnPool));
        PendingConnPool *pool = (PendingConnPool *)t->http_pending_conns;
        pool->epoll_fd = epoll_create1(0);
        if (pool->epoll_fd < 0) {
            fprintf(stderr, "  http: epoll_create1() failed: %s\n", strerror(errno));
        }
        /* Phase 4: initialize io_uring (non-TLS only) */
        int ret = io_uring_queue_init(512, &pool->ring, 0);
        pool->use_io_uring = (ret == 0);
        if (!pool->use_io_uring) {
            fprintf(stderr, "  http: io_uring_queue_init() failed (%d), falling back to epoll\n", ret);
        }
    }
    return (PendingConnPool *)t->http_pending_conns;
}

static void pending_pool_add(PendingConnPool *pool, int fd, const char *ip) {
    if (pool->count >= MAX_PENDING_CONNS) {
        close(fd); /* hard cap reached -- drop rather than grow unbounded */
        return;
    }
    if (pool->count >= pool->capacity) {
        int new_cap = pool->capacity ? pool->capacity * 2 : 16;
        pool->conns = (PendingConn *)realloc(pool->conns, (size_t)new_cap * sizeof(PendingConn));
        pool->capacity = new_cap;
    }
    PendingConn *pc = &pool->conns[pool->count++];
    pc->fd = fd;
    snprintf(pc->ip, sizeof(pc->ip), "%s", ip);
    conn_buffer_init(&pc->buf);
    pc->last_activity = now_seconds();
    pc->request_count = 0;
    pc->ssl = NULL;
    pc->tls_handshaking = false;
    pc->io_uring_pending = false;
    pc->io_uring_recv_off = 0;
    if (pool->tls_ctx) {
        pc->ssl = SSL_new(pool->tls_ctx);
        SSL_set_fd(pc->ssl, fd);
        pc->tls_handshaking = true;
    }
    /* Phase 4: in io_uring mode, only add TLS connections to epoll.
     * Plain HTTP connections use io_uring recv SQEs instead. */
    bool needs_epoll = true;
    if (pool->use_io_uring && !pc->ssl) needs_epoll = false;
    if (needs_epoll && pool->epoll_fd >= 0) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = fd;
        epoll_ctl(pool->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
}

/* transferred=true means a handler took ownership of the raw fd (WebSocket/
 * SSE) -- don't close it or free its SSL session, just stop tracking it
 * here. Swap-with-last, so callers must iterate the pool backwards while
 * removing. */
static void pending_pool_remove(PendingConnPool *pool, int i, bool transferred) {
    PendingConn *pc = &pool->conns[i];
    if (!transferred) {
        if (pc->ssl) { SSL_shutdown(pc->ssl); SSL_free(pc->ssl); }
        /* Phase 2: remove from epoll before closing */
        if (pool->epoll_fd >= 0)
            epoll_ctl(pool->epoll_fd, EPOLL_CTL_DEL, pc->fd, NULL);
        close(pc->fd);
    }
    conn_buffer_free(&pc->buf);
    pool->conns[i] = pool->conns[pool->count - 1];
    pool->count--;
}

void http_cleanup_pending_conns(Task *t) {
    PendingConnPool *pool = (PendingConnPool *)t->http_pending_conns;
    if (!pool) return;
    if (pool->epoll_fd >= 0) close(pool->epoll_fd);
    /* Phase 4: tear down io_uring */
    if (pool->use_io_uring) io_uring_queue_exit(&pool->ring);
    for (int i = 0; i < pool->count; i++) {
        if (pool->conns[i].ssl) { SSL_shutdown(pool->conns[i].ssl); SSL_free(pool->conns[i].ssl); }
        close(pool->conns[i].fd);
        conn_buffer_free(&pool->conns[i].buf);
    }
    if (pool->route_trie) radix_node_free(pool->route_trie);
    if (pool->tls_ctx) SSL_CTX_free(pool->tls_ctx);
    free(pool->conns);
    free(pool);
    t->http_pending_conns = NULL;
    /* Cluster worker threads (see spawn_cluster_workers) each own a fully
     * independent VM/Task/pool -- this task's pool teardown has no bearing
     * on them, and they run for the lifetime of the process either way. */
}

static void send_plain_status(int fd, SSL *ssl, int status, const char *status_text) {
    char resp[160];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, status_text);
    if (n > 0 && n < (int)sizeof(resp)) conn_io_send_all(fd, ssl, resp, n);
}

/* One non-blocking pass over every pending connection: advance a pending
 * TLS handshake if there is one, read whatever's available, process a
 * complete request if one is framed, enforce size limits and the idle
 * timeout, and drop connections that are done. */
static void poll_pending_connections(VM *vm, PendingConnPool *pool, Value handler_or_routes_val, bool is_routed) {
    double now = now_seconds();

    /* ─── Phase 4: io_uring — process completions (accept + recv) ─── */
    if (pool->use_io_uring) {
        io_uring_submit(&pool->ring);
        struct io_uring_cqe *cqe;
        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&pool->ring, head, cqe) {
            count++;
            int fd = (int)(uintptr_t)io_uring_cqe_get_data(cqe);
            int res = cqe->res;

            if (fd == -1) {
                /* Accept completion */
                if (res >= 0) {
                    int client_fd = res;
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    if (flags >= 0) fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
                    int nodelay = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    char ip_str[64] = "127.0.0.1";
                    struct sockaddr_in peer;
                    socklen_t peer_len = sizeof(peer);
                    if (getpeername(client_fd, (struct sockaddr *)&peer, &peer_len) == 0) {
                        unsigned char *ip = (unsigned char *)&peer.sin_addr.s_addr;
                        snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
                    }
                    pending_pool_add(pool, client_fd, ip_str);
                    vm->io_activity_this_tick = true;
                }
                /* Re-submit accept SQE */
                struct io_uring_sqe *sqe = io_uring_get_sqe(&pool->ring);
                if (sqe) {
io_uring_prep_accept(sqe, pool->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    io_uring_sqe_set_data(sqe, (void *)(uintptr_t)(-1));
                }
            } else {
                /* Recv completion for fd */
                for (int i = 0; i < pool->count; i++) {
                    PendingConn *pc = &pool->conns[i];
                    if (pc->fd == fd && !pc->ssl) {
                        pc->io_uring_pending = false;
                        if (res > 0) {
                            /* The kernel wrote res bytes at the offset that was
                             * current when this SQE was submitted
                             * (io_uring_recv_off), NOT necessarily at the
                             * connection's current buf.len: conn_buffer_consume()
                             * can shrink buf.len (via memmove, draining a
                             * pipelined request) while this recv was still
                             * in flight. Reconcile by copying the newly
                             * received bytes down to the current end of
                             * valid data before extending len, rather than
                             * assuming src and dest offsets coincide. */
                            int src_off = pc->io_uring_recv_off;
                            int dest_off = pc->buf.len;
                            int new_len = dest_off + res;
                            if (new_len >= pc->buf.capacity) {
                                int new_cap = pc->buf.capacity;
                                while (new_cap < new_len + 1) new_cap *= 2;
                                int cap_limit = MAX_REQUEST_HEADER_SIZE + MAX_REQUEST_BODY_SIZE + 1;
                                if (new_cap > cap_limit) new_cap = cap_limit;
                                char *nd = (char *)realloc(pc->buf.data, (size_t)new_cap);
                                if (nd) { pc->buf.data = nd; pc->buf.capacity = new_cap; }
                            }
                            if (dest_off != src_off) {
                                memmove(pc->buf.data + dest_off, pc->buf.data + src_off, (size_t)res);
                            }
                            pc->buf.len = new_len;
                            pc->buf.data[new_len] = '\0';
                            pc->last_activity = now;
                            vm->io_activity_this_tick = true;
                        } else {
                            /* res == 0 (EOF) or res < 0 (error) */
                            pending_pool_remove(pool, i, false);
                        }
                        break;
                    }
                }
            }
        }
        io_uring_cq_advance(&pool->ring, count);

        /* Submit recv SQEs for connections without an outstanding recv */
        for (int i = 0; i < pool->count; i++) {
            PendingConn *pc = &pool->conns[i];
            if (!pc->io_uring_pending && !pc->tls_handshaking && !pc->ssl) {
                struct io_uring_sqe *sqe = io_uring_get_sqe(&pool->ring);
                if (sqe) {
                    /* Ensure buffer has room */
                    if (pc->buf.len + 1 >= pc->buf.capacity) {
                        int new_cap = pc->buf.capacity * 2;
                        int cap_limit = MAX_REQUEST_HEADER_SIZE + MAX_REQUEST_BODY_SIZE + 1;
                        if (new_cap > cap_limit) new_cap = cap_limit;
                        if (new_cap > pc->buf.capacity) {
                            char *nd = (char *)realloc(pc->buf.data, (size_t)new_cap);
                            if (nd) { pc->buf.data = nd; pc->buf.capacity = new_cap; }
                        }
                    }
                    int want = pc->buf.capacity - pc->buf.len - 1;
                    if (want > 0) {
                        io_uring_prep_recv(sqe, pc->fd, pc->buf.data + pc->buf.len, (unsigned)want, 0);
                        io_uring_sqe_set_data(sqe, (void *)(uintptr_t)(intptr_t)pc->fd);
                        pc->io_uring_pending = true;
                        pc->io_uring_recv_off = pc->buf.len;
                    }
                }
            }
        }
        /* Flush SQEs so recv/accept operate immediately */
        io_uring_submit(&pool->ring);
    }

    /* ─── Fallback: epoll + sync recv (TLS connections and no-io_uring) ─── */
    #define MAX_EPOLL_EVENTS 256
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int nfds = 0;
    if (!pool->use_io_uring && pool->epoll_fd >= 0) {
        nfds = epoll_wait(pool->epoll_fd, events, MAX_EPOLL_EVENTS, 0);
        if (nfds > 0) vm->io_activity_this_tick = true;
    } else if (pool->use_io_uring && pool->epoll_fd >= 0) {
        /* In io_uring mode, only check epoll for TLS connections */
        nfds = epoll_wait(pool->epoll_fd, events, MAX_EPOLL_EVENTS, 0);
    }

    for (int i = pool->count - 1; i >= 0; i--) {
        PendingConn *pc = &pool->conns[i];
        bool is_io_uring = pool->use_io_uring && !pc->ssl;

        /* In io_uring mode, skip synchronous recv — handled by completions.
         * Only TLS connections still use the synchronous epoll path. */
        bool needs_sync_recv = false;
        if (is_io_uring) {
            /* Check if io_uring recv completed for this fd */
            needs_sync_recv = false; /* io_uring completions already advanced buf.len */
        } else {
            /* Check whether epoll reported this fd as ready */
            if (nfds > 0) {
                for (int e = 0; e < nfds; e++) {
                    if (events[e].data.fd == pc->fd) { needs_sync_recv = true; break; }
                }
            } else if (nfds < 0) {
                needs_sync_recv = true;
            }
        }

        if (pc->tls_handshaking) {
            if (!needs_sync_recv) {
                if (now - pc->last_activity > CONN_IDLE_TIMEOUT_SEC)
                    pending_pool_remove(pool, i, false);
                continue;
            }
            int r = SSL_accept(pc->ssl);
            if (r == 1) {
                pc->tls_handshaking = false;
                pc->last_activity = now;
            } else {
                int err = SSL_get_error(pc->ssl, r);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    if (now - pc->last_activity > CONN_IDLE_TIMEOUT_SEC)
                        pending_pool_remove(pool, i, false);
                    continue;
                }
                pending_pool_remove(pool, i, false);
                continue;
            }
        }

        /* For non-io_uring connections (TLS), do synchronous recv */
        if (!is_io_uring && needs_sync_recv) {
            int r = conn_buffer_recv_more(pc->fd, pc->ssl, &pc->buf, MAX_REQUEST_HEADER_SIZE + MAX_REQUEST_BODY_SIZE);
            if (r > 0) {
                pc->last_activity = now;
            } else if (r == 0 || r == -2) {
                pending_pool_remove(pool, i, false);
                continue;
            }
        }

        int total_len = 0;
        ReqFrameStatus status = try_frame_request(&pc->buf, &total_len);
        if (status == REQ_INCOMPLETE) {
            /* For io_uring connections with incomplete data, ensure we have an outstanding recv */
            if (is_io_uring && !pc->io_uring_pending) {
                struct io_uring_sqe *sqe = io_uring_get_sqe(&pool->ring);
                if (sqe) {
                    if (pc->buf.len + 1 >= pc->buf.capacity) {
                        int new_cap = pc->buf.capacity * 2;
                        int cap_limit = MAX_REQUEST_HEADER_SIZE + MAX_REQUEST_BODY_SIZE + 1;
                        if (new_cap > cap_limit) new_cap = cap_limit;
                        if (new_cap > pc->buf.capacity) {
                            char *nd = (char *)realloc(pc->buf.data, (size_t)new_cap);
                            if (nd) { pc->buf.data = nd; pc->buf.capacity = new_cap; }
                        }
                    }
                    int want = pc->buf.capacity - pc->buf.len - 1;
                    if (want > 0) {
                        io_uring_prep_recv(sqe, pc->fd, pc->buf.data + pc->buf.len, (unsigned)want, 0);
                        io_uring_sqe_set_data(sqe, (void *)(uintptr_t)(intptr_t)pc->fd);
                        pc->io_uring_pending = true;
                        pc->io_uring_recv_off = pc->buf.len;
                    }
                }
            }
            if (now - pc->last_activity > CONN_IDLE_TIMEOUT_SEC)
                pending_pool_remove(pool, i, false);
            continue;
        }
        if (status == REQ_HEADERS_TOO_LARGE) {
            send_plain_status(pc->fd, pc->ssl, 431, "Request Header Fields Too Large");
            pending_pool_remove(pool, i, false);
            continue;
        }
        if (status == REQ_BODY_TOO_LARGE) {
            send_plain_status(pc->fd, pc->ssl, 413, "Payload Too Large");
            pending_pool_remove(pool, i, false);
            continue;
        }

        /* REQ_READY */
        vm->io_activity_this_tick = true;
        pc->request_count++;
        bool transferred = false;
        bool keep_alive = handle_connection(vm, pc->fd, pc->ssl, pc->ip, pc->buf.data, total_len,
                                             pc->request_count, handler_or_routes_val, is_routed,
                                             pool->route_trie, &transferred);
        if (transferred) {
            pending_pool_remove(pool, i, true);
            continue;
        }
        conn_buffer_consume(&pc->buf, total_len);
        if (!keep_alive) {
            pending_pool_remove(pool, i, false);
            continue;
        }
        pc->last_activity = now;
    }
}

/* Loads a cert+key pair into a fresh server-side SSL_CTX. NULL (with
 * runtime_error already called) on any failure -- a bad/missing cert
 * should never half-start a listener. */
static SSL_CTX *create_tls_context(VM *vm, const char *cert_path, const char *key_path, const char *fn_name) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        runtime_error(vm, "%s(): SSL_CTX_new() failed", fn_name);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
        runtime_error(vm, "%s(): failed to load certificate '%s'", fn_name, cert_path);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
        runtime_error(vm, "%s(): failed to load private key '%s'", fn_name, key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        runtime_error(vm, "%s(): certificate/private key mismatch ('%s' / '%s')", fn_name, cert_path, key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

/* cert_path/key_path NULL = plain HTTP listener; both non-NULL = HTTPS. */
static bool http_serve_setup_listener(VM *vm, Task *t, int port, int workers, const char *fn_name,
                                       const char *cert_path, const char *key_path) {
    if (t->http_listen_fd != -1) return true;

    SSL_CTX *tls_ctx = NULL;
    if (cert_path && key_path) {
        tls_ctx = create_tls_context(vm, cert_path, key_path, fn_name);
        if (!tls_ctx) return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        runtime_error(vm, "%s(): socket() failed", fn_name);
        if (tls_ctx) SSL_CTX_free(tls_ctx);
        return false;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    /* Required for cluster mode: each worker thread (see
     * spawn_cluster_workers) runs its own fully independent VM and binds
     * its OWN socket on this same port rather than sharing one fd -- the
     * kernel load-balances incoming connections across all of them. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        runtime_error(vm, "%s(): bind() failed on port %d", fn_name, port);
        close(fd);
        if (tls_ctx) SSL_CTX_free(tls_ctx);
        return false;
    }
    if (listen(fd, 128) < 0) {
        runtime_error(vm, "%s(): listen() failed", fn_name);
        close(fd);
        if (tls_ctx) SSL_CTX_free(tls_ctx);
        return false;
    }
    printf("  %s: listening on port %d%s\n", fn_name, port, tls_ctx ? " (TLS)" : "");
    fflush(stdout);
    t->http_listen_fd = fd;
    PendingConnPool *pool = get_pending_pool(t);
    pool->tls_ctx = tls_ctx;
    pool->listen_fd = fd;
    spawn_cluster_workers(workers, port);
    return true;
}

/* Shared by http.serve/http.serve_with_routes: poll every pending
 * connection once, then accept new ones (added to the pool, polled
 * starting next tick rather than processed inline -- one extra tick of
 * latency, at most ~1ms on an otherwise-idle server, in exchange for one
 * uniform code path for "a connection has a complete request ready"). */
static bool http_serve_tick(VM *vm, Task *t, Value handler_or_routes_val, bool is_routed, const char *fn_name) {
    PendingConnPool *pool = get_pending_pool(t);

    /* Phase 4: submit accept SQEs to io_uring if available */
    if (pool->use_io_uring) {
        for (int i = 0; i < 8; i++) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&pool->ring);
            if (!sqe) break;
            io_uring_prep_accept(sqe, pool->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            io_uring_sqe_set_data(sqe, (void *)(uintptr_t)(-1));
        }
    } else {
        /* Fallback: synchronous accept for non-io_uring (TLS) listeners */
        for (int burst = 0; burst < 16; burst++) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(t->http_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (client_fd >= 0) {
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
                int nodelay = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                char ip_str[64] = "127.0.0.1";
                unsigned char *ip = (unsigned char *)&client_addr.sin_addr.s_addr;
                snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
                pending_pool_add(pool, client_fd, ip_str);
                vm->io_activity_this_tick = true;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                runtime_error(vm, "%s(): accept() error", fn_name);
                close(t->http_listen_fd);
                t->http_listen_fd = -1;
                return false;
            }
        }
    }

    poll_pending_connections(vm, pool, handler_or_routes_val, is_routed);
    return true;
}

/* ═══════════════════════════════════════════
 *  Thread-based worker cluster
 *
 *  lib_http_serve() is a cooperative native function: it yields and gets
 *  retried every scheduler tick (see its t->yielded = true at the end), so
 *  by the time a script reaches app.listen_cluster(port, workers), its
 *  whole top level has already run -- every route is registered, every
 *  closure built. That means the only way to get a *second* independent
 *  copy of that same ready-to-serve state is to either (a) actually share
 *  the one VM's heap across OS threads (which would require making the
 *  single-threaded mark-sweep GC and every allocation site thread-safe --
 *  a much larger, riskier rewrite than this), or (b) give each worker
 *  thread its own VM and have it independently re-parse and re-compile the
 *  exact same script from scratch, reaching the identical ready-to-serve
 *  state on its own. This does (b): each worker thread is a fully isolated
 *  VM/heap/GC, so there is zero shared mutable VM state between threads,
 *  and therefore no locking needed anywhere in the interpreter for this to
 *  be correct. The only process-wide (non-per-VM) mutable state that
 *  multiple threads' independent scripts could ever race on is guarded
 *  separately (see lib_cache_lock in ffi.c, curl_global_init() in main.c).
 *
 *  Each worker binds its own socket with SO_REUSEPORT on the same port
 *  (see http_serve_setup_listener) rather than sharing one listening fd --
 *  the kernel itself load-balances new connections across every bound
 *  socket, so there's no single shared accept() queue to contend on, and
 *  no accept() thundering-herd between workers either.
 * ═══════════════════════════════════════════ */

#define MAX_CLUSTER_WORKERS 64

/* Set true at the top of cluster_worker_thread_main(), before that thread's
 * independent VM ever starts running bytecode. If the script it's running
 * reaches its own app.listen_cluster(...) call, this stops it from trying
 * to spawn yet another whole set of worker threads recursively -- only the
 * original (non-worker) thread that first called listen_cluster() spawns
 * workers; every worker just serves on its own SO_REUSEPORT socket. */
static __thread bool g_is_cluster_worker_thread = false;

static int g_cluster_worker_count = 0;

typedef struct {
    int id;
} ClusterWorkerArg;

static void *cluster_worker_thread_main(void *arg) {
    ClusterWorkerArg *cwa = (ClusterWorkerArg *)arg;
    int id = cwa->id;
    free(cwa);
    g_is_cluster_worker_thread = true;

    if (!g_varian_script_path) {
        fprintf(stderr, "  cluster worker %d: no script path recorded, exiting\n", id);
        return NULL;
    }
    char *source = read_file_with_modules(g_varian_script_path);
    if (!source) {
        fprintf(stderr, "  cluster worker %d: could not reload '%s'\n", id, g_varian_script_path);
        return NULL;
    }

    Lexer lexer;
    lexer_init(&lexer, source, g_varian_script_path);
    Arena *arena = arena_create(0);
    Parser parser;
    parser_init(&parser, &lexer, arena);
    AstNode *program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "  cluster worker %d: parse error: %s\n", id, parser_get_error(&parser));
        arena_destroy(arena);
        free(source);
        return NULL;
    }

    Chunk chunk;
    chunk_init(&chunk);
    Compiler compiler;
    compiler_init(&compiler, arena, &chunk, program);
    if (!compiler_compile(&compiler)) {
        fprintf(stderr, "  cluster worker %d: compile error: %s\n", id, compiler.error_message);
        chunk_free(&chunk);
        arena_destroy(arena);
        free(source);
        return NULL;
    }

    VM vm;
    vm_init(&vm, &compiler);
    vm_run(&vm, false); /* this worker's own copy of the server's event loop -- runs forever */
    vm_free(&vm);
    chunk_free(&chunk);
    arena_destroy(arena);
    free(source);
    return NULL;
}

static void spawn_cluster_workers(int worker_count, int port) {
    if (worker_count <= 1) return;
    if (g_is_cluster_worker_thread) return; /* a worker's own nested run must not spawn more */
    for (int i = 0; i < worker_count - 1 && i < MAX_CLUSTER_WORKERS; i++) {
        ClusterWorkerArg *cwa = (ClusterWorkerArg *)malloc(sizeof(ClusterWorkerArg));
        cwa->id = i + 1;
        pthread_t th;
        if (pthread_create(&th, NULL, cluster_worker_thread_main, cwa) != 0) {
            fprintf(stderr, "  http.serve: pthread_create() failed for worker %d: %s\n", i + 1, strerror(errno));
            free(cwa);
            break;
        }
        pthread_detach(th);
        g_cluster_worker_count++;
    }
    printf("  http.serve: %d worker thread(s) on port %d (pid %d)\n",
           g_cluster_worker_count + 1, port, getpid());
    fflush(stdout);
}

/* ═══════════════════════════════════════════
 *  http.serve(port, handler_fn [, workers])
 * ═══════════════════════════════════════════ */

static Value lib_http_serve(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 3 && args[0].type == VAL_MODULE);
    int port;
    Value handler_val;
    int workers = 1;
    if (is_dispatch) {
        if (args[1].type != VAL_INT || (args[2].type != VAL_FUNCTION && args[2].type != VAL_CLOSURE)) {
            runtime_error(vm, "http.serve() requires port (int) and handler (function)");
            return val_nil();
        }
        port = (int)args[1].as.integer;
        handler_val = args[2];
        if (arg_count >= 4 && args[3].type == VAL_INT) workers = (int)args[3].as.integer;
    } else {
        if (arg_count < 2 || args[0].type != VAL_INT || (args[1].type != VAL_FUNCTION && args[1].type != VAL_CLOSURE)) {
            runtime_error(vm, "http.serve() requires port (int) and handler (function)");
            return val_nil();
        }
        port = (int)args[0].as.integer;
        handler_val = args[1];
        if (arg_count >= 3 && args[2].type == VAL_INT) workers = (int)args[2].as.integer;
    }
    if (workers < 1) workers = 1;
    Task *t = vm->current_task;
    if (!http_serve_setup_listener(vm, t, port, workers, "http.serve", NULL, NULL)) {
        return val_nil();
    }
    if (!http_serve_tick(vm, t, handler_val, false, "http.serve")) {
        return val_nil();
    }
    /* Rewind to retry this call next scheduler tick. BC_DISPATCH is
     * opcode(1) + method_name constant idx(2) + arg_count(1) = 4 bytes;
     * a plain BC_CALL (handler stored in a variable, called directly)
     * is opcode(1) + arg_count(1) = 2 bytes. */
    if (is_dispatch)
        t->frames[t->frame_count - 1].ip -= 4;
    else
        t->frames[t->frame_count - 1].ip -= 2;
    t->yielded = true;
    return val_nil();
}

/* ═══════════════════════════════════════════
 *  http.serve_tls(port, handler_fn, cert_path, key_path [, workers])
 *
 *  Same multiplexed, non-blocking accept/keep-alive machinery as
 *  http.serve() -- the only difference is the listening socket gets an
 *  SSL_CTX (see http_serve_setup_listener), and pending_pool_add() wraps
 *  every accepted connection in an SSL session before anything is read.
 *  TLS handshakes are advanced non-blockingly in poll_pending_connections,
 *  the same one-cheap-check-per-tick design as everything else here -- a
 *  slow TLS handshake costs no more than a slow plaintext request does.
 * ═══════════════════════════════════════════ */

static Value lib_http_serve_tls(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 5 && args[0].type == VAL_MODULE);
    int port;
    Value handler_val;
    const char *cert_path;
    const char *key_path;
    int workers = 1;
    if (is_dispatch) {
        if (args[1].type != VAL_INT || (args[2].type != VAL_FUNCTION && args[2].type != VAL_CLOSURE) ||
            args[3].type != VAL_STRING || args[4].type != VAL_STRING) {
            runtime_error(vm, "http.serve_tls() requires port (int), handler (function), cert_path (string), key_path (string)");
            return val_nil();
        }
        port = (int)args[1].as.integer;
        handler_val = args[2];
        cert_path = args[3].as.string->chars;
        key_path = args[4].as.string->chars;
        if (arg_count >= 6 && args[5].type == VAL_INT) workers = (int)args[5].as.integer;
    } else {
        if (arg_count < 4 || args[0].type != VAL_INT || (args[1].type != VAL_FUNCTION && args[1].type != VAL_CLOSURE) ||
            args[2].type != VAL_STRING || args[3].type != VAL_STRING) {
            runtime_error(vm, "http.serve_tls() requires port (int), handler (function), cert_path (string), key_path (string)");
            return val_nil();
        }
        port = (int)args[0].as.integer;
        handler_val = args[1];
        cert_path = args[2].as.string->chars;
        key_path = args[3].as.string->chars;
        if (arg_count >= 5 && args[4].type == VAL_INT) workers = (int)args[4].as.integer;
    }
    if (workers < 1) workers = 1;
    Task *t = vm->current_task;
    if (!http_serve_setup_listener(vm, t, port, workers, "http.serve_tls", cert_path, key_path)) {
        return val_nil();
    }
    if (!http_serve_tick(vm, t, handler_val, false, "http.serve_tls")) {
        return val_nil();
    }
    if (is_dispatch)
        t->frames[t->frame_count - 1].ip -= 4;
    else
        t->frames[t->frame_count - 1].ip -= 2;
    t->yielded = true;
    return val_nil();
}

/* ═══════════════════════════════════════════
 *  http.serve_with_routes(port, routes_array)
 * ═══════════════════════════════════════════ */

static Value lib_http_serve_with_routes(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 3 && args[0].type == VAL_MODULE);
    int port;
    Value routes_val;
    if (is_dispatch) {
        if (args[1].type != VAL_INT || args[2].type != VAL_ARRAY) {
            runtime_error(vm, "http.serve_with_routes() requires port (int) and routes (array)");
            return val_nil();
        }
        port = (int)args[1].as.integer;
        routes_val = args[2];
    } else {
        if (arg_count < 2 || args[0].type != VAL_INT || args[1].type != VAL_ARRAY) {
            runtime_error(vm, "http.serve_with_routes() requires port (int) and routes (array)");
            return val_nil();
        }
        port = (int)args[0].as.integer;
        routes_val = args[1];
    }
    Task *t = vm->current_task;
    if (!http_serve_setup_listener(vm, t, port, 1, "http.serve_with_routes", NULL, NULL)) {
        return val_nil();
    }

    /* Phase 3: Build radix trie from routes array */
    PendingConnPool *pool = get_pending_pool(t);
    if (routes_val.type == VAL_ARRAY) {
        ObjArray *routes = routes_val.as.array;
        pool->route_trie = radix_node_new("", 0);
        for (int i = 0; i < routes->count; i++) {
            if (routes->elements[i].type != VAL_STRUCT) continue;
            ObjStruct *route = routes->elements[i].as.structure;
            const char *r_method = NULL;
            const char *r_path = NULL;
            Value r_handler = val_nil();
            for (int j = 0; j < route->field_count; j++) {
                if (strcmp(route->field_names[j], "method") == 0 && route->fields[j].type == VAL_STRING)
                    r_method = route->fields[j].as.string->chars;
                if (strcmp(route->field_names[j], "path") == 0 && route->fields[j].type == VAL_STRING)
                    r_path = route->fields[j].as.string->chars;
                if (strcmp(route->field_names[j], "handler") == 0 &&
                    (route->fields[j].type == VAL_FUNCTION || route->fields[j].type == VAL_CLOSURE))
                    r_handler = route->fields[j];
            }
            if (r_method && r_path && r_handler.type != VAL_NIL) {
                radix_insert(pool->route_trie, r_method, r_path, r_handler);
            }
        }
    }

    if (!http_serve_tick(vm, t, routes_val, true, "http.serve_with_routes")) {
        return val_nil();
    }
    if (is_dispatch)
        t->frames[t->frame_count - 1].ip -= 4;
    else
        t->frames[t->frame_count - 1].ip -= 2;
    t->yielded = true;
    return val_nil();
}

/* ─── http.create_struct(keys, values) ─── */
static Value lib_http_create_struct(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 3 && args[0].type == VAL_MODULE);
    Value keys_val = is_dispatch ? args[1] : args[0];
    Value vals_val = is_dispatch ? args[2] : args[1];

    if (keys_val.type != VAL_ARRAY || vals_val.type != VAL_ARRAY) {
        runtime_error(vm, "http.create_struct() requires keys (array) and values (array)");
        return val_nil();
    }
    ObjArray *keys = keys_val.as.array;
    ObjArray *vals = vals_val.as.array;
    int count = keys->count < vals->count ? keys->count : vals->count;
    ObjStruct *s = new_struct(vm, count, false);
    s->type_name = NULL;
    for (int i = 0; i < count; i++) {
        if (keys->elements[i].type == VAL_STRING) {
            s->field_names[i] = strdup(keys->elements[i].as.string->chars);
        } else {
            s->field_names[i] = strdup("");
        }
        s->fields[i] = vals->elements[i];
    }
    return val_struct(s);
}

/* ─── http.test_request(method, path, body) ─── */
/* Builds the same request shape handle_connection() builds from a real
 * socket, so app.handle(http.test_request(...)) exercises the exact route +
 * middleware path a live request would, with no socket involved. */
static Value lib_http_test_request(VM *vm, int arg_count, Value *args) {
    int base = http_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_STRING || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "http.test_request(method, path, body, headers) requires method (string) and path (string)");
        return val_nil();
    }
    const char *method = args[base].as.string->chars;
    const char *path = args[base + 1].as.string->chars;
    Value body_val = (arg_count > base + 2) ? args[base + 2] : val_nil();
    Value headers_val = (arg_count > base + 3) ? args[base + 3] : val_nil();
    Value json_val = val_nil();
    if (body_val.type == VAL_STRING) {
        json_val = json_decode(vm, body_val.as.string->chars);
    }
    return make_request(vm, method, path, body_val, json_val, val_nil(), headers_val, "127.0.0.1", -1);
}

/* ─── http.write_socket(fd, data) ─── */
static Value lib_http_write_socket(VM *vm, int arg_count, Value *args) {
    int base = http_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_INT || args[base + 1].type != VAL_STRING) {
        runtime_error(vm, "http.write_socket(fd, data) requires fd (int) and data (string)");
        return val_nil();
    }
    int fd = (int)args[base].as.integer;
    ObjString *data = args[base + 1].as.string;
    int sent = (int)send(fd, data->chars, (size_t)data->length, 0);
    return val_int(sent);
}

/* ─── http.close_socket(fd) ─── */
static Value lib_http_close_socket(VM *vm, int arg_count, Value *args) {
    int base = http_arg_base(arg_count, args);
    if (arg_count < base + 1 || args[base].type != VAL_INT) {
        runtime_error(vm, "http.close_socket(fd) requires fd (int)");
        return val_nil();
    }
    int fd = (int)args[base].as.integer;
    close(fd);
    return val_nil();
}

/* ─── http.read_socket(fd, max_bytes) ─── */
static Value lib_http_read_socket(VM *vm, int arg_count, Value *args) {
    int base = http_arg_base(arg_count, args);
    if (arg_count < base + 2 || args[base].type != VAL_INT || args[base + 1].type != VAL_INT) {
        runtime_error(vm, "http.read_socket(fd, max_bytes) requires fd (int) and max_bytes (int)");
        return val_nil();
    }
    int fd = (int)args[base].as.integer;
    int max_bytes = (int)args[base + 1].as.integer;
    if (max_bytes <= 0) return val_string(allocate_string(vm, "", 0));
    
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    char *buf = malloc((size_t)max_bytes);
    if (!buf) return val_nil();
    
    int n = (int)recv(fd, buf, (size_t)max_bytes, 0);
    if (n < 0) {
        free(buf);
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return val_string(allocate_string(vm, "", 0));
        }
        return val_nil();
    } else if (n == 0) {
        free(buf);
        return val_nil();
    }
    
    ObjString *result = allocate_string(vm, buf, n);
    free(buf);
    return val_string(result);
}

/* ─── Registration ─── */
void lib_http_init(VM *vm) {
    ObjModule *mod = new_module("http");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("http", 4), val_module(mod));
    vm_register_dispatch(vm, "http", "get",    val_native_fn((void *)lib_http_get));
    vm_register_dispatch(vm, "http", "post",   val_native_fn((void *)lib_http_post));
    vm_register_dispatch(vm, "http", "serve",  val_native_fn((void *)lib_http_serve));
    vm_register_dispatch(vm, "http", "serve_tls", val_native_fn((void *)lib_http_serve_tls));
    vm_register_dispatch(vm, "http", "serve_with_routes", val_native_fn((void *)lib_http_serve_with_routes));
    vm_register_dispatch(vm, "http", "create_struct", val_native_fn((void *)lib_http_create_struct));
    vm_register_dispatch(vm, "http", "test_request", val_native_fn((void *)lib_http_test_request));
    vm_register_dispatch(vm, "http", "write_socket", val_native_fn((void *)lib_http_write_socket));
    vm_register_dispatch(vm, "http", "close_socket", val_native_fn((void *)lib_http_close_socket));
    vm_register_dispatch(vm, "http", "read_socket",  val_native_fn((void *)lib_http_read_socket));
}
