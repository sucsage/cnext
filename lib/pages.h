#ifndef PAGES_H
#define PAGES_H

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

// =====================================================================
// PageFn — signature ของ page function
// =====================================================================
typedef void (*PageFn)(HttpRequest *req, PageCtx *ctx);

// =====================================================================
// add_page  — register page (ใช้ได้จากทุกที่)
// pages_init — parse shell.html (เรียกใน main ก่อน start server)
// =====================================================================
void add_page  (const char *method, const char *route, PageFn fn);
void pages_init(void);

// Pre-build HTTP response ทั้งหมด (headers+shell+content) ตอน startup
// ใช้กับ page ที่ไม่มี dynamic data — serve ด้วย memcpy เดียว ไม่ผ่าน page_handler
// เรียกหลัง pages_init() เสมอ
void add_static_page(const char *route, const char *html_content);

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
