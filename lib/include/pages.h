#ifndef PAGES_H
#define PAGES_H

#include <string.h>
#include "server.h"

// =====================================================================
// PageCtx — output buffer ของแต่ละ page
// =====================================================================
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} PageCtx;

void page_write (PageCtx *ctx, const char *html);
void page_writef(PageCtx *ctx, const char *fmt, ...);

// Fast path for compile-time-known lengths (string literals).
// cxnc emits page_write_n(ctx, "…", sizeof("…")-1) so the length
// is a constant — no strlen at runtime, and small memcpy can be
// inlined into direct stores by the compiler.
static inline void page_write_n(PageCtx *ctx, const char *s, size_t n) {
    if (ctx->len + n >= ctx->cap) return;
    memcpy(ctx->buf + ctx->len, s, n);
    ctx->len += n;
    ctx->buf[ctx->len] = '\0';
}

// =====================================================================
// PageFn — signature ของ page function
// =====================================================================
typedef void (*PageFn)(HttpRequest *req, PageCtx *ctx);

// =====================================================================
// add_page  — register page (ใช้ได้จากทุกที่)
// pages_init — เรียกใน main ก่อน start server
// =====================================================================
void add_page  (const char *method, const char *route, PageFn fn);
void pages_init(void);

// =====================================================================
// Layout — generated from src/layout.cxn (Next.js-style)
//
// ครอบทุก page โดยอัตโนมัติ ตรงจุด {{children}} ใน layout.cxn
// ถ้าไม่มี layout.cxn ใช้ weak default ใน pages.c (เรียก child ตรงๆ)
// =====================================================================
void app_layout(HttpRequest *req, PageCtx *ctx, PageFn child);

// =====================================================================
// REGISTER_PAGE — เหมือน Next.js App Router
//
// วางตรงท้ายของทุก page.c:
//   static void render(HttpRequest *req, PageCtx *ctx) { ... }
//   REGISTER_PAGE("/route")
//
// __attribute__((constructor)) ทำให้ register ก่อน main() เลย
// =====================================================================
#define _PAGE_CONCAT(a, b) a##b
#define _PAGE_CTOR(line)   _PAGE_CONCAT(__page_ctor_, line)

#define REGISTER_PAGE(route)                                    \
    __attribute__((constructor))                                \
    static void _PAGE_CTOR(__LINE__)(void) {                    \
        add_page("GET", (route), render);                       \
    }

#endif // PAGES_H
