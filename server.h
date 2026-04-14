#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include <stddef.h> // สำหรับ size_t

// โครงสร้างข้อมูล Request
typedef struct {
    char method[10];
    char path[255];
    char user_agent[512];
    char cookie[2048];
    char body[2048];
} HttpRequest;

// 1. กำหนดรูปแบบหน้าตาของ Function (Function Pointer) ที่จะมารับงาน
typedef void (*ApiHandler)(HttpRequest *req, int socket_fd);

// 2. โครงสร้าง Route สำหรับเก็บเส้นทาง API
typedef struct {
    char *method;
    char *path;
    ApiHandler handler;
} Route;

// ฟังก์ชันหลักของ Server
int setup_server(int port);
void accept_clients(int server_fd, int port);

// ฟังก์ชันสำหรับ Router
void register_routes(Route *routes, size_t count);

// ฟังก์ชันตัวช่วย (Helper) สำหรับส่ง Response แบบง่ายๆ
void send_response(int socket_fd, int status_code, const char *status_text, const char *content_type, const char *body);

#endif // SERVER_H