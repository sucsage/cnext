#include <stdio.h>
#include "server.h"
#include "db/db.h"
#include "pages.h"
#include "static.h"

#define PORT    8080
#define DB_PATH "./data"
#define DB_SIZE 1          // GB

int main(void) {
    if (db_init(DB_PATH, DB_SIZE) != 0) {
        fprintf(stderr, "[cnext] db_init failed\n");
        return 1;
    }

    pages_init();

    static_init();  // cache public/ เข้า RAM

    int server_fd = setup_server(PORT);
    accept_clients(server_fd, PORT);

    db_close();
    return 0;
}
