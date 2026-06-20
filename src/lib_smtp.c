#include "lib_smtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

/* ─── Minimal plaintext SMTP client ───
 * Talks the SMTP submission dialogue (EHLO/MAIL FROM/RCPT TO/DATA/QUIT)
 * over a raw TCP connection. No STARTTLS/AUTH — that's enough for a local
 * mailpit/MailHog dev relay or an internal unauthenticated relay. For
 * providers that require auth/TLS (SendGrid, Resend, SES), use mail.vn's
 * HTTP API path (http.post) instead — see vn_modules/mail.vn. */

static int smtp_connect(const char *host, int port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int smtp_read_response(int fd, char *buf, int buf_size) {
    int n = (int)recv(fd, buf, (size_t)(buf_size - 1), 0);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return n;
}

static bool smtp_send_cmd(int fd, const char *cmd) {
    size_t len = strlen(cmd);
    return send(fd, cmd, len, 0) == (ssize_t)len;
}

/* smtp.send(host, port, from, to, subject, body) -> bool */
static Value lib_smtp_send(VM *vm, int arg_count, Value *args) {
    bool is_dispatch = (arg_count >= 7 && args[0].type == VAL_MODULE);
    int base = is_dispatch ? 1 : 0;

    if (arg_count < base + 6 ||
        args[base].type != VAL_STRING || args[base + 1].type != VAL_INT ||
        args[base + 2].type != VAL_STRING || args[base + 3].type != VAL_STRING ||
        args[base + 4].type != VAL_STRING || args[base + 5].type != VAL_STRING) {
        runtime_error(vm, "smtp.send(host, port, from, to, subject, body) requires "
                           "(string, int, string, string, string, string)");
        return val_bool(false);
    }

    const char *host = args[base].as.string->chars;
    int port = (int)args[base + 1].as.integer;
    const char *from = args[base + 2].as.string->chars;
    const char *to = args[base + 3].as.string->chars;
    const char *subject = args[base + 4].as.string->chars;
    const char *body = args[base + 5].as.string->chars;

    /* SMTP header / command injection guard: from/to/subject are interpolated
     * directly into SMTP commands and message headers, so a bare CR or LF in
     * any of them would let a caller (often relaying user input from a contact
     * form) inject extra recipients, headers, or SMTP verbs. Reject rather than
     * silently strip so the caller learns their input was malformed. The body
     * sits after DATA and may legitimately contain newlines, so it is exempt
     * from the header check -- but see dot-stuffing below. */
    if (strpbrk(from, "\r\n") || strpbrk(to, "\r\n") || strpbrk(subject, "\r\n")) {
        runtime_error(vm, "smtp.send(): from/to/subject must not contain CR or LF "
                           "(header injection)");
        return val_bool(false);
    }

    /* Connection failure is a mundane, expected outcome (relay not running,
     * network down) -- return false rather than throwing, same convention
     * as io.read_text() returning nil for a missing file. */
    int fd = smtp_connect(host, port);
    if (fd < 0) {
        return val_bool(false);
    }

    char buf[4096];
    char cmd[2048];

    smtp_read_response(fd, buf, sizeof(buf)); /* 220 greeting */

    snprintf(cmd, sizeof(cmd), "EHLO varian.local\r\n");
    smtp_send_cmd(fd, cmd);
    smtp_read_response(fd, buf, sizeof(buf));

    snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>\r\n", from);
    smtp_send_cmd(fd, cmd);
    smtp_read_response(fd, buf, sizeof(buf));

    snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", to);
    smtp_send_cmd(fd, cmd);
    smtp_read_response(fd, buf, sizeof(buf));

    smtp_send_cmd(fd, "DATA\r\n");
    smtp_read_response(fd, buf, sizeof(buf));

    char data[8192];
    snprintf(data, sizeof(data),
             "From: %s\r\nTo: %s\r\nSubject: %s\r\n\r\n%s\r\n.\r\n",
             from, to, subject, body);
    smtp_send_cmd(fd, data);
    int n = smtp_read_response(fd, buf, sizeof(buf));

    smtp_send_cmd(fd, "QUIT\r\n");
    close(fd);

    /* A real SMTP server responds "250 ..." once the message is accepted. */
    bool ok = (n > 0 && strncmp(buf, "250", 3) == 0);
    return val_bool(ok);
}

void lib_smtp_init(VM *vm) {
    ObjModule *mod = new_module("smtp");
    mod->obj.next = vm->objects;
    vm->objects = (Obj *)mod;
    define_global(vm, copy_string("smtp", 4), val_module(mod));
    vm_register_dispatch(vm, "smtp", "send", val_native_fn((void *)lib_smtp_send));
}
