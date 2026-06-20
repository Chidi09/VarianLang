#ifndef LIB_HTTP_H
#define LIB_HTTP_H

#include "vm.h"

void lib_http_init(VM *vm);

/* Called by the VM's round-robin scheduler once a deferred long-lived HTTP
 * handler task (see call_handler/http_response_fd in lib_http.c) finishes. */
void http_finalize_deferred_response(Task *t, bool had_error);

/* Called when a task that owns an http.serve() pending-connection pool
 * dies, to close every still-open fd and free the pool itself. */
void http_cleanup_pending_conns(Task *t);

#endif /* LIB_HTTP_H */
