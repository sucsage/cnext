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
// PageMeta — per-page metadata declared via @meta in .cxn files
// =====================================================================
typedef struct {
    const char *title;
    const char *description;
    const char *og_image;
    const char *canonical;
} PageMeta;

// =====================================================================
// add_page       — register page without metadata
// add_page_meta  — register page with optional PageMeta (NULL = no meta)
// pages_init     — เรียกใน main ก่อน start server
// =====================================================================
void add_page     (const char *method, const char *route, PageFn fn);
void add_page_meta(const char *method, const char *route, PageFn fn,
                   const PageMeta *meta);
void pages_init   (void);

// ── Layout helpers ────────────────────────────────────────────────────
// Returns the active page's PageMeta (or NULL if the page had no @meta).
// Safe to call from inside the generated app_layout() body.
const PageMeta *page_current_meta(void);

// Convenience: emit <title> + <meta name="description"> + og:* tags
// based on the current page's metadata. Falls back to "cnext" as the
// title when no @meta title was declared.
void page_emit_meta_head(PageCtx *ctx);

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
