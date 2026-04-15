#include "pages.h"

// ไม่มี dynamic data → ใช้ add_static_page() ใน main.c แทน
// response ถูก pre-build ตอน startup → serve ด้วย memcpy เดียว

#define INDEX_HTML \
    "<section class='hero'>" \
      "<div class='hero-badge'>Phase 3</div>" \
      "<h1 class='hero-title'>cnext</h1>" \
      "<p class='hero-sub'>C server &bull; io_uring &bull; SSR like Next.js</p>" \
      "<div class='tech-grid'>" \
        "<div class='tech-card'>" \
          "<div class='tech-icon'>&#9889;</div>" \
          "<div class='tech-name'>io_uring</div>" \
          "<div class='tech-desc'>Async I/O แบบ zero-copy</div>" \
        "</div>" \
        "<div class='tech-card'>" \
          "<div class='tech-icon'>&#128190;</div>" \
          "<div class='tech-name'>LMDB</div>" \
          "<div class='tech-desc'>Cold storage บน disk</div>" \
        "</div>" \
        "<div class='tech-card'>" \
          "<div class='tech-icon'>&#9889;</div>" \
          "<div class='tech-name'>Hot Cache</div>" \
          "<div class='tech-desc'>RAM tier + TTL</div>" \
        "</div>" \
        "<div class='tech-card'>" \
          "<div class='tech-icon'>&#128196;</div>" \
          "<div class='tech-name'>C Pages</div>" \
          "<div class='tech-desc'>src/page.c &#8594; GET /</div>" \
        "</div>" \
      "</div>" \
      "<div class='cta-row'>" \
        "<a href='/user' class='btn-primary'>Users &rarr;</a>" \
        "<a href='/api/users' class='btn-secondary'>API JSON</a>" \
      "</div>" \
    "</section>"

// export ให้ main.c เรียก add_static_page("/", INDEX_HTML)
const char *page_index_html = INDEX_HTML;
