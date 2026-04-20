/* POST /action/counter/{inc,reset} — HTMX-style server action.
 *
 * Unlike a redirect-style action, HTMX expects a fragment back. We update
 * session state then return the counter fragment HTML directly so HTMX
 * can swap it into #counter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "server.h"
#include "session.h"
#include "pages.h"
#include "src/counter/fragment_cxn.h"   /* cxn_fragment_counter_fragment */

static void respond_with_fragment(HttpRequest *req, int socket_fd) {
    char  buf[4096];
    PageCtx ctx = { .buf = buf, .len = 0, .cap = sizeof(buf) };
    cxn_fragment_counter_fragment(req, &ctx);
    ctx.buf[ctx.len] = '\0';
    send_response(socket_fd, 200, "OK", "text/html; charset=utf-8", ctx.buf);
}

static void action(HttpRequest *req, int socket_fd) {
    if (strstr(req->path, "/reset")) {
        session_del(req, "count");
        respond_with_fragment(req, socket_fd);
        return;
    }

    int  n = 0;
    char nb[16];
    char *cur = session_get(req, "count");
    if (cur) {
        n = atoi(cur);
        free(cur);
        cur = NULL;
    }
    n++;
    snprintf(nb, sizeof(nb), "%d", n);
    session_set(req, "count", nb, 0);

    respond_with_fragment(req, socket_fd);
}

__attribute__((constructor))
static void _counter_action_ctor(void) {
    Route rs[] = {
        { "POST", "/action/counter/inc",   action },
        { "POST", "/action/counter/reset", action },
    };
    register_routes(rs, 2);
}
