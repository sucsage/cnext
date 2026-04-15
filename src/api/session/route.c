#include "route.h"
#include "db/db.h"
#include <stdio.h>
#include <stdlib.h>

static void get(HttpRequest *req, int socket_fd) {
    const char *token = req->path + 13; // ตัด "/api/session/" ออก
    char *sess = db_get("sessions", token);
    if (sess) {
        send_response(socket_fd, 200, "OK", "application/json", sess);
        free(sess);
    } else {
        send_response(socket_fd, 404, "Not Found",
                      "application/json", "{\"error\":\"session not found\"}\n");
    }
}

static void post(HttpRequest *req, int socket_fd) {
    const char *token = "tok_abc123";
    db_put(DB_HOT, "sessions", token, req->body);
    db_ttl("sessions", token, 1800);

    char resp[256];
    snprintf(resp, sizeof(resp), "{\"token\":\"%s\"}\n", token);
    send_response(socket_fd, 201, "Created", "application/json", resp);
}

REGISTER_GET("/api/session")
REGISTER_POST("/api/session")
