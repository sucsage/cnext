#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "server.h"
#include "db/db.h"

#define PORT    8080
#define DB_PATH "./data"
#define DB_SIZE 1          // GB

// ---------------------------------------------------------
// Controllers
// ---------------------------------------------------------

void hello_world(HttpRequest *req, int socket_fd) {
    (void)req;
    send_response(socket_fd, 200, "OK", "text/plain", "Hello from cnext!\n");
}

// GET /api/users — ดึงจาก DB (hot → cold อัตโนมัติ)
void get_users(HttpRequest *req, int socket_fd) {
    (void)req;
    char *data = db_get("users", "all");
    if (data) {
        send_response(socket_fd, 200, "OK", "application/json", data);
        free(data);
    } else {
        send_response(socket_fd, 200, "OK", "application/json",
                      "[{\"id\": 1, \"name\": \"Sage\"}]\n");
    }
}

// POST /api/users — เก็บลง DB ทั้งสอง tier
void create_user(HttpRequest *req, int socket_fd) {
    if (!req->body[0]) {
        send_response(socket_fd, 400, "Bad Request",
                      "application/json", "{\"error\":\"empty body\"}\n");
        return;
    }

    // เก็บ body ลงทั้ง RAM และ disk
    db_put(DB_BOTH, "users", "all", req->body);

    char resp[4096];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"created\",\"received\":%s}\n", req->body);
    send_response(socket_fd, 201, "Created", "application/json", resp);
}

// GET /api/session/:token — ตัวอย่าง hot-only (RAM, TTL 30 นาที)
void get_session(HttpRequest *req, int socket_fd) {
    // ดึง token จาก path (simplified)
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

// POST /api/session — สร้าง session ใน RAM เท่านั้น (TTL 30 นาที)
void create_session(HttpRequest *req, int socket_fd) {
    const char *token = "tok_abc123"; // ในงานจริงใช้ random token
    db_put(DB_HOT, "sessions", token, req->body);
    db_ttl("sessions", token, 1800); // หมดอายุ 30 นาที

    char resp[256];
    snprintf(resp, sizeof(resp), "{\"token\":\"%s\"}\n", token);
    send_response(socket_fd, 201, "Created", "application/json", resp);
}

// ---------------------------------------------------------
// Routes
// ---------------------------------------------------------
Route my_api_routes[] = {
    {"GET",  "/",                hello_world   },
    {"GET",  "/api/users",       get_users     },
    {"POST", "/api/users",       create_user   },
    {"GET",  "/api/session",     get_session   },
    {"POST", "/api/session",     create_session},
};

// ---------------------------------------------------------
// Main
// ---------------------------------------------------------
int main(void) {
    // 1. เปิด database (hot RAM + cold LMDB)
    if (db_init(DB_PATH, DB_SIZE) != 0) {
        fprintf(stderr, "[cnext] db_init failed\n");
        return 1;
    }

    // 2. ลงทะเบียน routes
    size_t route_count = sizeof(my_api_routes) / sizeof(Route);
    register_routes(my_api_routes, route_count);

    // 3. เปิด server
    int server_fd = setup_server(PORT);
    accept_clients(server_fd, PORT);

    db_close();
    return 0;
}
