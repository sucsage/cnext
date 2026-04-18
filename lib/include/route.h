#ifndef ROUTE_H
#define ROUTE_H

#include "server.h"

// =====================================================================
// REGISTER_* — เหมือน Next.js App Router
//
// วางตรงท้ายของทุก route.c:
//   static void get (HttpRequest *req, int socket_fd) { ... }
//   static void post(HttpRequest *req, int socket_fd) { ... }
//   REGISTER_GET("/api/users")
//   REGISTER_POST("/api/users")
//
// __attribute__((constructor)) ทำให้ register ก่อน main() เลย
// =====================================================================

// indirection สองชั้น — บังคับให้ __LINE__ expand ก่อน concat
#define _ROUTE_CONCAT(a, b) a##b
#define _ROUTE_CTOR(prefix, line) _ROUTE_CONCAT(prefix, line)

#define REGISTER_GET(path) \
    __attribute__((constructor)) \
    static void _ROUTE_CTOR(__route_get_,  __LINE__)(void) { \
        Route r = {"GET",    (path), get};  register_routes(&r, 1); \
    }

#define REGISTER_POST(path) \
    __attribute__((constructor)) \
    static void _ROUTE_CTOR(__route_post_, __LINE__)(void) { \
        Route r = {"POST",   (path), post}; register_routes(&r, 1); \
    }

#define REGISTER_PUT(path) \
    __attribute__((constructor)) \
    static void _ROUTE_CTOR(__route_put_,  __LINE__)(void) { \
        Route r = {"PUT",    (path), put};  register_routes(&r, 1); \
    }

#define REGISTER_DELETE(path) \
    __attribute__((constructor)) \
    static void _ROUTE_CTOR(__route_del_,  __LINE__)(void) { \
        Route r = {"DELETE", (path), delete};  register_routes(&r, 1); \
    }

// Server Action — POST handler named "action" → process แล้ว redirect
// วางท้าย action*.c:
//   static void action(HttpRequest *req, int socket_fd) { ...; send_redirect(...); }
//   REGISTER_ACTION("/action/users/create")
#define REGISTER_ACTION(path) \
    __attribute__((constructor)) \
    static void _ROUTE_CTOR(__route_action_, __LINE__)(void) { \
        Route r = {"POST", (path), action}; register_routes(&r, 1); \
    }

#endif // ROUTE_H
