#include "pages.h"
#include "db/db.h"
#include <stdio.h>
#include <stdlib.h>

#define CACHE_NS  "cache"
#define CACHE_KEY "users:list"

typedef struct {
    char   buf[65536];
    size_t len;
    int    first;
} ListCtx;

static void render(HttpRequest *req, PageCtx *ctx) {
    (void)req;

    // SSR — ดึงข้อมูลจาก DB ก่อน render เหมือน getServerSideProps
    char *data = db_list("users");;
    const char *users_json = data ? data : "[{\"id\":1,\"name\":\"Sage\"}]";
    

    page_writef(ctx,
        "<section class='page'>"
          "<div class='page-header'><h1>Users</h1></div>"
          "<form id='f' class='create-form'>"
            "<input id='n' type='text' placeholder='ชื่อผู้ใช้' required autocomplete='off'>"
            "<button type='submit' class='btn-primary'>+ Add</button>"
          "</form>"
          "<div id='list' class='user-list'></div>"
          "<script>"
            "function render(users){"
              "document.getElementById('list').innerHTML=users.map(u=>"
                "`<div class='user-card'>"
                  "<div class='user-avatar'>${(u.name||'?')[0].toUpperCase()}</div>"
                  "<div class='user-info'><strong>${u.name}</strong><small>#${u.id}</small></div>"
                "</div>`).join('')"
            "}"
            "render(%s);"
            "document.getElementById('f').onsubmit=async e=>{"
              "e.preventDefault();"
              "const name=document.getElementById('n').value.trim();"
              "if(!name)return;"
              "await fetch('/api/users',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name,id:Date.now()})});"
              "document.getElementById('n').value='';"
              "render(await(await fetch('/api/users')).json());"
            "};"
          "</script>"
        "</section>",
        users_json
    );

    if (data) free(data);
}

REGISTER_PAGE("/user")
