#include "route.h"
#include "db/db.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void get(HttpRequest *req, int socket_fd) {
    (void)req;
    char *list = db_list("users");
    send_response(socket_fd, 200, "OK", "application/json", list ? list : "[]\n");
    free(list);
}

static void post(HttpRequest *req, int socket_fd) {
    if (!req->body[0]) {
        send_response(socket_fd, 400, "Bad Request",
                      "application/json", "{\"error\":\"empty body\"}\n");
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char id[32];
    snprintf(id, sizeof(id), "%ld%09ld", (long)ts.tv_sec, ts.tv_nsec);

    // inject id เข้าไปใน object → {"id":"...","name":"Test"}
    char data[4096];
    snprintf(data, sizeof(data), "{\"id\":\"%s\",%s", id, req->body + 1);
    db_put(DB_COLD, "users", id, data);

    char resp[sizeof(data) + 64];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"created\",\"data\":%s}\n", data);
    send_response(socket_fd, 201, "Created", "application/json", resp);
}


static void delete(HttpRequest *req, int socket_fd) {
    const char *id = req->path + 11;   // ข้าม "/api/users/"
    if (!id[0]) {
        send_response(socket_fd, 400, "Bad Request",
                      "application/json", "{\"error\":\"missing id\"}\n");
        return;
    }
    char *existing = db_get("users", id);
    if (!existing) {
        send_response(socket_fd, 404, "Not Found",
                      "application/json", "{\"error\":\"user not found\"}\n");
        return;
    }
    free(existing);
    db_del("users", id);
    send_response(socket_fd, 200, "OK", "application/json", "{\"status\":\"deleted\"}\n");
}


REGISTER_GET("/api/users")
REGISTER_POST("/api/users")
REGISTER_DELETE("/api/users")