#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "pages.h"
#include "server.h"

#define PAGE_BUF    (56 * 1024)  // content buffer per worker thread (layout+page)
#define MAX_PAGES   64

// =====================================================================
// Page Registry
// =====================================================================
typedef struct {
    char   method[10];
    char   route[256];
    PageFn fn;
} PageEntry;

static PageEntry entries[MAX_PAGES];
static int       entry_count = 0;

// TLS page buffer — shared across shell + content, no malloc per request
static __thread char tls_page_buf[PAGE_BUF];

// =====================================================================
// Default layout — used when src/layout.cxn is absent.
// Strong definition from generated layout_cxn.c overrides this.
// =====================================================================
__attribute__((weak))
void app_layout(HttpRequest *req, PageCtx *ctx, PageFn child) {
    if (child) child(req, ctx);
}

// =====================================================================
// PageCtx API
// =====================================================================
void page_write(PageCtx *ctx, const char *html) {
    size_t n = strlen(html);
    if (ctx->len + n >= ctx->cap) return;
    memcpy(ctx->buf + ctx->len, html, n);
    ctx->len += n;
    ctx->buf[ctx->len] = '\0';
}

void page_writef(PageCtx *ctx, const char *fmt, ...) {
    if (ctx->len >= ctx->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ctx->buf + ctx->len, ctx->cap - ctx->len, fmt, ap);
    va_end(ap);
    if (n > 0) ctx->len += (size_t)n < ctx->cap - ctx->len
                            ? (size_t)n : ctx->cap - ctx->len - 1;
}

// =====================================================================
// Universal Page Handler
// =====================================================================
static void page_handler(HttpRequest *req, int socket_fd) {
    PageFn fn = NULL;
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].method, req->method) == 0 &&
            strcmp(entries[i].route,  req->path)   == 0) {
            fn = entries[i].fn; break;
        }
    }
    if (!fn) {
        send_response(socket_fd, 404, "Not Found", "text/plain", "Page not found\n");
        return;
    }

    PageCtx ctx = { .buf = tls_page_buf, .len = 0, .cap = PAGE_BUF };
    ctx.buf[0] = '\0';
    app_layout(req, &ctx, fn);
    ctx.buf[ctx.len] = '\0';

    send_response(socket_fd, 200, "OK", "text/html; charset=utf-8", ctx.buf);
}

// =====================================================================
// add_page — register ทันที (ปลอดภัยเรียกจาก constructor ก่อน main)
// =====================================================================
void add_page(const char *method, const char *route, PageFn fn) {
    if (entry_count >= MAX_PAGES) return;
    PageEntry *pe = &entries[entry_count++];
    strncpy(pe->method, method, sizeof(pe->method) - 1);
    strncpy(pe->route,  route,  sizeof(pe->route)  - 1);
    pe->fn = fn;

    Route r = { .method = pe->method, .path = pe->route, .handler = page_handler };
    register_routes(&r, 1);
}

// =====================================================================
// pages_init — called from main before server starts
// =====================================================================
void pages_init(void) {
    printf("[pages] %d page(s) registered\n", entry_count);
}
