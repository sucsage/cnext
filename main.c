#include <stdio.h>
#include "server.h"
#include "db/db.h"
#include "pages.h"
#include "static.h"

#define PORT    8080
#define DB_PATH "./data"
#define DB_SIZE 1          // GB

// pre-built HTML content จาก src/page.c
extern const char *page_index_html;

int main(void) {
    if (db_init(DB_PATH, DB_SIZE) != 0) {
        fprintf(stderr, "[cnext] db_init failed\n");
        return 1;
    }

    pages_init();   // parse layouts/shell.html

    // Static pages — pre-build HTTP response ทั้งหมดตอน startup
    // GET / → memcpy เดียว ไม่ผ่าน page_handler เลย
    add_static_page("/", page_index_html);

    static_init();  // cache public/ เข้า RAM

    int server_fd = setup_server(PORT);
    accept_clients(server_fd, PORT);

    db_close();
    return 0;
}
