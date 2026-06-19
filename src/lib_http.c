#include "lib_http.h"
#include "json.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdbool.h>

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

/* ─── http.get(url) ─── */
static Value lib_http_get(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != VAL_STRING) return val_nil();
    const char *url = args[0].as.string->chars;
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

/* ═══════════════════════════════════════════
 *  HTTP Request Parsing
 * ═══════════════════════════════════════════ */

static bool parse_http_request(const char *raw, char *method, int method_size,
                                char *path, int path_size) {
    const char *end = strstr(raw, "\r\n");
    if (!end) end = raw + strlen(raw);
    const char *sp = strchr(raw, ' ');
    if (!sp || sp >= end) return false;
    int mlen = (int)(sp - raw);
    if (mlen >= method_size) mlen = method_size - 1;
    memcpy(method, raw, (size_t)mlen);
    method[mlen] = '\0';
    sp++;
    const char *sp2 = strchr(sp, ' ');
    if (!sp2 || sp2 > end) sp2 = end;
    int plen = (int)(sp2 - sp);
    if (plen >= path_size) plen = path_size - 1;
    memcpy(path, sp, (size_t)plen);
    path[plen] = '\0';
    return method[0] != '\0' && path[0] != '\0';
}

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

static const char *find_body_start(const char *buf) {
    const char *body = strstr(buf, "\r\n\r\n");
    if (!body) return NULL;
    return body + 4;
}

/* Read full HTTP request, handling chunked body via Content-Length */
static int read_http_request(int client_fd, char *buf, int buf_size) {
    int n = 0;
    while (n < buf_size - 1) {
        int r = (int)recv(client_fd, buf + n, (size_t)(buf_size - 1 - n), 0);
        if (r <= 0) break;
        n += r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (n <= 0) return n;
    buf[n] = '\0';
    const char *body_start = find_body_start(buf);
    if (!body_start) return n;
    int body_received = (int)((buf + n) - body_start);
    const char *cl = find_header_value(buf, "Content-Length");
    if (!cl) return n;
    int content_length = atoi(cl);
    while (body_received < content_length && n < buf_size - 1) {
        int r = (int)recv(client_fd, buf + n, (size_t)(buf_size - 1 - n), 0);
        if (r <= 0) break;
        n += r;
        buf[n] = '\0';
        body_received += r;
    }
    return n;
}

/* ═══════════════════════════════════════════
 *  Struct Creation Helpers
 * ═══════════════════════════════════════════ */

static Value make_request(VM *vm, const char *method, const char *path,
                           Value body_val, Value json_val, Value params_val) {
    int total = 5;
    ObjStruct *req = (ObjStruct *)calloc(1, sizeof(ObjStruct));
    req->obj.type = VAL_STRUCT;
    req->field_count = total;
    req->field_names = (char **)calloc((size_t)total, sizeof(char *));
    req->fields = (Value *)calloc((size_t)total, sizeof(Value));
    req->type_name = NULL;

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

    return val_struct(req);
}

static void free_request(Value req_val) {
    if (req_val.type != VAL_STRUCT) return;
    ObjStruct *req = req_val.as.structure;
    if (!req) return;
    for (int i = 0; i < req->field_count; i++)
        free(req->field_names[i]);
    free(req->field_names);
    free(req->fields);
    free(req);
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

static Value make_params_struct(VM *vm, const char *path,
                                 char *param_names[], int param_name_lens[],
                                 int param_starts[], int param_lens[],
                                 int param_count) {
    if (param_count <= 0) return val_nil();
    ObjStruct *ps = (ObjStruct *)calloc(1, sizeof(ObjStruct));
    ps->obj.type = VAL_STRUCT;
    ps->field_count = param_count;
    ps->field_names = (char **)calloc((size_t)param_count, sizeof(char *));
    ps->fields = (Value *)calloc((size_t)param_count, sizeof(Value));
    ps->type_name = NULL;
    for (int i = 0; i < param_count; i++) {
        char *name = (char *)malloc((size_t)param_name_lens[i] + 1);
        memcpy(name, param_names[i], (size_t)param_name_lens[i]);
        name[param_name_lens[i]] = '\0';
        ps->field_names[i] = name;
        char *val_str = (char *)malloc((size_t)param_lens[i] + 1);
        memcpy(val_str, path + param_starts[i], (size_t)param_lens[i]);
        val_str[param_lens[i]] = '\0';
        ps->fields[i] = val_string(allocate_string(vm, val_str, param_lens[i]));
        free(val_str);
    }
    return val_struct(ps);
}

/* ═══════════════════════════════════════════
 *  Response Sending
 * ═══════════════════════════════════════════ */

static void send_http_response(int client_fd, Value result) {
    int status = 200;
    const char *body = NULL;
    int body_len = 0;
    const char *content_type = "text/plain";
    if (result.type == VAL_STRING) {
        body = result.as.string->chars;
        body_len = (int)strlen(body);
    } else if (result.type == VAL_STRUCT) {
        ObjStruct *rs = result.as.structure;
        for (int i = 0; i < rs->field_count; i++) {
            if (strcmp(rs->field_names[i], "status") == 0 && rs->fields[i].type == VAL_INT)
                status = (int)rs->fields[i].as.integer;
            if (strcmp(rs->field_names[i], "body") == 0 && rs->fields[i].type == VAL_STRING) {
                body = rs->fields[i].as.string->chars;
                body_len = (int)strlen(body);
            }
            if (strcmp(rs->field_names[i], "content_type") == 0 && rs->fields[i].type == VAL_STRING)
                content_type = rs->fields[i].as.string->chars;
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
    char resp[16384];
    int rn = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %d\r\n"
        "Content-Type: %s\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, status_desc, body_len, content_type, body);
    if (rn > 0 && rn < (int)sizeof(resp))
        send(client_fd, resp, (size_t)rn, 0);
}

/* ═══════════════════════════════════════════
 *  Handler Executor
 * ═══════════════════════════════════════════ */

static Value call_handler(VM *vm, Value handler_val, Value req_val) {
    ObjFunction *handler_fn = handler_val.as.function;
    Task *tmp = task_new(vm);
    tmp->stack[tmp->stack_top++] = req_val;
    tmp->frames[0].function = handler_fn;
    tmp->frames[0].ip = handler_fn->code;
    tmp->frames[0].slots = tmp->stack;
    tmp->frame_count = 1;
    Task *prev = vm->current_task;
    vm->current_task = tmp;
    task_run(vm, tmp);
    vm->current_task = prev;
    Value result = val_nil();
    if (tmp->stack_top > 0)
        result = tmp->stack[tmp->stack_top - 1];
    tmp->dead = true;
    return result;
}

/* ═══════════════════════════════════════════
 *  Handle a single connection (shared by serve and serve_with_routes)
 *  If routes_val is not nil, it's a VAL_ARRAY of route structs to match
 * ═══════════════════════════════════════════ */

static void handle_connection(VM *vm, int client_fd, Value handler_or_routes_val,
                               bool is_routed) {
    char buf[65536];
    int n = read_http_request(client_fd, buf, (int)sizeof(buf));
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';
    char method[64], path[2048];
    if (!parse_http_request(buf, method, sizeof(method), path, sizeof(path))) {
        close(client_fd);
        return;
    }
    /* Extract body */
    const char *body_str = find_body_start(buf);
    int body_len = 0;
    Value body_val = val_nil();
    if (body_str) {
        body_len = (int)((buf + n) - body_str);
        if (body_len > 0)
            body_val = val_string(allocate_string(vm, body_str, body_len));
    }
    /* JSON parse if Content-Type is application/json */
    Value json_val = val_nil();
    const char *ct = find_header_value(buf, "Content-Type");
    if (ct && body_len > 0) {
        const char *ct_end = strstr(ct, "\r\n");
        int ct_len = ct_end ? (int)(ct_end - ct) : (int)strlen(ct);
        (void)ct_len;
        if (strstr(ct, "application/json")) {
            char *json_buf = (char *)malloc((size_t)body_len + 1);
            memcpy(json_buf, body_str, (size_t)body_len);
            json_buf[body_len] = '\0';
            json_val = json_decode(vm, json_buf);
            free(json_buf);
        }
    }
    Value result = val_nil();
    if (is_routed) {
        /* Iterate routes to find a match */
        Value routes_val = handler_or_routes_val;
        bool found = false;
        if (routes_val.type == VAL_ARRAY) {
            ObjArray *routes = routes_val.as.array;
            for (int i = 0; i < routes->count; i++) {
                if (routes->elements[i].type != VAL_STRUCT) continue;
                ObjStruct *route = routes->elements[i].as.structure;
                const char *route_method = NULL;
                const char *route_path = NULL;
                Value route_handler = val_nil();
                for (int j = 0; j < route->field_count; j++) {
                    if (strcmp(route->field_names[j], "method") == 0 && route->fields[j].type == VAL_STRING)
                        route_method = route->fields[j].as.string->chars;
                    if (strcmp(route->field_names[j], "path") == 0 && route->fields[j].type == VAL_STRING)
                        route_path = route->fields[j].as.string->chars;
                    if (strcmp(route->field_names[j], "handler") == 0 && route->fields[j].type == VAL_FUNCTION)
                        route_handler = route->fields[j];
                }
                if (!route_method || !route_path || route_handler.type == VAL_NIL) continue;
                /* Match method (case-insensitive) */
                if (strcasecmp(route_method, method) != 0) continue;
                /* Match path */
                char *param_names[MAX_PARAMS];
                int param_name_lens[MAX_PARAMS];
                int param_starts[MAX_PARAMS];
                int param_lens[MAX_PARAMS];
                int param_count = 0;
                if (!match_route(route_path, path,
                                 param_names, param_name_lens,
                                 param_starts, param_lens,
                                 &param_count)) continue;
                found = true;
                Value params_val = make_params_struct(vm, path,
                                                       param_names, param_name_lens,
                                                       param_starts, param_lens,
                                                       param_count);
                Value req_val = make_request(vm, method, path, body_val, json_val, params_val);
                result = call_handler(vm, route_handler, req_val);
                free_request(req_val);
                break;
            }
        }
        if (!found) {
            result = val_nil();
            /* Build a 404 response struct manually */
            ObjStruct *rs = (ObjStruct *)calloc(1, sizeof(ObjStruct));
            rs->obj.type = VAL_STRUCT;
            rs->field_count = 2;
            rs->field_names = (char **)calloc(2, sizeof(char *));
            rs->fields = (Value *)calloc(2, sizeof(Value));
            rs->type_name = NULL;
            rs->field_names[0] = strdup("status");
            rs->fields[0] = val_int(404);
            rs->field_names[1] = strdup("body");
            rs->fields[1] = val_string(allocate_string(vm, "Not Found", 9));
            Value not_found = val_struct(rs);
            send_http_response(client_fd, not_found);
            /* Clean up the not_found struct */
            free(rs->field_names[0]);
            free(rs->field_names[1]);
            free(rs->field_names);
            free(rs->fields);
            free(rs);
            close(client_fd);
            return;
        }
    } else {
        Value req_val = make_request(vm, method, path, body_val, json_val, val_nil());
        result = call_handler(vm, handler_or_routes_val, req_val);
        free_request(req_val);
    }
    send_http_response(client_fd, result);
    close(client_fd);
}

/* ═══════════════════════════════════════════
 *  http.serve(port, handler_fn)
 * ═══════════════════════════════════════════ */

static Value lib_http_serve(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 3 && args[0].type == VAL_MODULE);
    int port;
    Value handler_val;
    if (is_dispatch) {
        if (args[1].type != VAL_INT || args[2].type != VAL_FUNCTION) {
            runtime_error(vm, "http.serve() requires port (int) and handler (function)");
            return val_nil();
        }
        port = (int)args[1].as.integer;
        handler_val = args[2];
    } else {
        if (arg_count < 2 || args[0].type != VAL_INT || args[1].type != VAL_FUNCTION) {
            runtime_error(vm, "http.serve() requires port (int) and handler (function)");
            return val_nil();
        }
        port = (int)args[0].as.integer;
        handler_val = args[1];
    }
    Task *t = vm->current_task;
    if (t->http_listen_fd == -1) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { runtime_error(vm, "http.serve(): socket() failed"); return val_nil(); }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            runtime_error(vm, "http.serve(): bind() failed on port %d", port); close(fd); return val_nil();
        }
        if (listen(fd, 128) < 0) {
            runtime_error(vm, "http.serve(): listen() failed"); close(fd); return val_nil();
        }
        printf("  http.serve: listening on port %d\n", port);
        t->http_listen_fd = fd;
    }
    for (int burst = 0; burst < 5; burst++) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(t->http_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd >= 0) {
            handle_connection(vm, client_fd, handler_val, false);
            close(client_fd);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            runtime_error(vm, "http.serve(): accept() error");
            close(t->http_listen_fd); t->http_listen_fd = -1;
            return val_nil();
        }
    }
    if (is_dispatch)
        t->frames[t->frame_count - 1].ip -= 3;
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
    if (t->http_listen_fd == -1) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { runtime_error(vm, "http.serve_with_routes(): socket() failed"); return val_nil(); }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)port);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            runtime_error(vm, "http.serve_with_routes(): bind() failed on port %d", port);
            close(fd); return val_nil();
        }
        if (listen(fd, 128) < 0) {
            runtime_error(vm, "http.serve_with_routes(): listen() failed");
            close(fd); return val_nil();
        }
        printf("  http.serve_with_routes: listening on port %d\n", port);
        t->http_listen_fd = fd;
    }
    for (int burst = 0; burst < 5; burst++) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(t->http_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd >= 0) {
            handle_connection(vm, client_fd, routes_val, true);
            close(client_fd);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            runtime_error(vm, "http.serve_with_routes(): accept() error");
            close(t->http_listen_fd); t->http_listen_fd = -1;
            return val_nil();
        }
    }
    if (is_dispatch)
        t->frames[t->frame_count - 1].ip -= 3;
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
    ObjStruct *s = new_struct(count);
    s->obj.next = vm->objects;
    vm->objects = (Obj *)s;
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

/* ─── Registration ─── */
void lib_http_init(VM *vm) {
    ObjModule *mod = new_module("http");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("http", 4), val_module(mod));
    vm_register_dispatch(vm, "http", "get",    val_native_fn((void *)lib_http_get));
    vm_register_dispatch(vm, "http", "serve",  val_native_fn((void *)lib_http_serve));
    vm_register_dispatch(vm, "http", "serve_with_routes", val_native_fn((void *)lib_http_serve_with_routes));
    vm_register_dispatch(vm, "http", "create_struct", val_native_fn((void *)lib_http_create_struct));
}
