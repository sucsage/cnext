#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "pages.h"
#include "server.h"

#define SHELL_PATH  "layouts/shell.html"
#define MARKER      "<!--CONTENT-->"
#define PAGE_BUF    (48 * 1024)  // content buffer per worker thread
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

// Shell (parse ครั้งเดียวตอน pages_init — shared read-only across all workers)
static char  *shell_head = NULL;  static size_t shell_head_len = 0;
static char  *shell_tail = NULL;  static size_t shell_tail_len = 0;

// TLS page buffers — ไม่ malloc ต่อ request
static __thread char tls_page_buf[PAGE_BUF];  // page function เขียนที่นี่
static __thread char tls_body_buf[PAGE_BUF + 8192]; // shell_head + content + shell_tail

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

    // ใช้ TLS buffers — ไม่ malloc ต่อ request
    PageCtx ctx = { .buf = tls_page_buf, .len = 0, .cap = PAGE_BUF };
    ctx.buf[0] = '\0';
    fn(req, &ctx);

    // ประกอบ response ลง TLS body buffer โดยตรง
    size_t body_cap = sizeof(tls_body_buf);
    size_t off = 0;

    if (shell_head && shell_head_len < body_cap - off) {
        memcpy(tls_body_buf + off, shell_head, shell_head_len); off += shell_head_len;
    }
    if (ctx.len < body_cap - off) {
        memcpy(tls_body_buf + off, ctx.buf, ctx.len); off += ctx.len;
    }
    if (shell_tail && shell_tail_len < body_cap - off) {
        memcpy(tls_body_buf + off, shell_tail, shell_tail_len); off += shell_tail_len;
    }
    tls_body_buf[off] = '\0';

    send_response(socket_fd, 200, "OK", "text/html; charset=utf-8", tls_body_buf);
}

// =====================================================================
// Static page cache — pre-built full HTTP response (headers + body)
// =====================================================================
#define MAX_STATIC_PAGES  32
#define STATIC_RESP_MAX   (96 * 1024)  // 96KB per cached response

typedef struct {
    char   route[256];
    char  *resp;     // สมบูรณ์ "HTTP/1.1 200 OK\r\n...\r\n\r\n<html>"
    size_t resp_len;
} StaticPage;

static StaticPage static_pages[MAX_STATIC_PAGES];
static int        static_page_count = 0;

static void static_page_handler(HttpRequest *req, int socket_fd) {
    (void)socket_fd;
    for (int i = 0; i < static_page_count; i++) {
        if (strcmp(static_pages[i].route, req->path) == 0) {
            // ส่ง pre-built response โดยตรง — bypass send_response ทั้งหมด
            set_raw_response(static_pages[i].resp, static_pages[i].resp_len);
            return;
        }
    }
    send_response(socket_fd, 404, "Not Found", "text/plain", "Page not found\n");
}

void add_static_page(const char *route, const char *html_content) {
    if (static_page_count >= MAX_STATIC_PAGES) return;
    if (!shell_head || !shell_tail) {
        fprintf(stderr, "[pages] add_static_page: call pages_init() first\n");
        return;
    }

    // ประกอบ full body: shell_head + content + shell_tail
    size_t content_len = strlen(html_content);
    size_t body_len    = shell_head_len + content_len + shell_tail_len;
    char  *body        = malloc(body_len + 1);
    if (!body) return;

    size_t off = 0;
    memcpy(body + off, shell_head, shell_head_len); off += shell_head_len;
    memcpy(body + off, html_content, content_len);  off += content_len;
    memcpy(body + off, shell_tail,   shell_tail_len); off += shell_tail_len;
    body[off] = '\0';

    // ประกอบ full HTTP response
    char *resp = malloc(STATIC_RESP_MAX);
    if (!resp) { free(body); return; }

    int hlen = snprintf(resp, STATIC_RESP_MAX,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
        body_len);

    size_t avail = STATIC_RESP_MAX - (size_t)hlen;
    size_t copy  = body_len < avail ? body_len : avail;
    memcpy(resp + hlen, body, copy);
    free(body);

    StaticPage *sp = &static_pages[static_page_count++];
    strncpy(sp->route, route, sizeof(sp->route) - 1);
    sp->resp     = resp;
    sp->resp_len = (size_t)hlen + copy;

    Route r = { .method = "GET", .path = sp->route, .handler = static_page_handler };
    register_routes(&r, 1);
    printf("[pages] static  %-20s pre-built %zu bytes\n", route, sp->resp_len);
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

    // register_routes เป็น additive — เรียกซ้ำได้
    Route r = { .method = pe->method, .path = pe->route, .handler = page_handler };
    register_routes(&r, 1);
}

// =====================================================================
// pages_init — parse shell.html (เรียกใน main ก่อน start server)
// =====================================================================
static char *file_read(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) { close(fd); return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (n < 0) { free(buf); return NULL; }
    buf[n] = '\0';
    if (out_len) *out_len = (size_t)n;
    return buf;
}

void pages_init(void) {
    size_t len = 0;
    char  *src = file_read(SHELL_PATH, &len);
    if (!src) { fprintf(stderr, "[pages] cannot read %s\n", SHELL_PATH); return; }

    char *split = strstr(src, MARKER);
    if (!split) { fprintf(stderr, "[pages] marker '%s' not found in %s\n", MARKER, SHELL_PATH); free(src); return; }

    shell_head_len = (size_t)(split - src);
    shell_head = malloc(shell_head_len + 1);
    memcpy(shell_head, src, shell_head_len);
    shell_head[shell_head_len] = '\0';

    char *tail_start = split + strlen(MARKER);
    shell_tail_len   = len - (size_t)(tail_start - src);
    shell_tail = malloc(shell_tail_len + 1);
    memcpy(shell_tail, tail_start, shell_tail_len);
    shell_tail[shell_tail_len] = '\0';

    free(src);
    printf("[pages] shell loaded  head=%zu tail=%zu bytes\n", shell_head_len, shell_tail_len);
    printf("[pages] %d page(s) registered\n", entry_count);
}
