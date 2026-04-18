#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>

// =====================================================================
// cxn_fetch — public HTTP fetch utility (blocking, wraps libcurl)
//
// Usage:
//   FetchResult *r = cxn_fetch("https://api.example.com/data", "GET", NULL);
//   if (r) { use(r->body, r->len, r->status); fetch_free(r); }
//
// เรียกจาก background thread เท่านั้น — จะ block จนกว่าจะได้ response
// =====================================================================

typedef struct {
    char  *body;    // response body (null-terminated)
    size_t len;     // body length
    int    status;  // HTTP status code (200, 404, etc.)
} FetchResult;

// method: "GET", "POST", "PUT", "DELETE", etc.
// body:   request body สำหรับ POST/PUT — NULL สำหรับ GET
// คืน NULL ถ้า network error หรือ OOM
FetchResult *cxn_fetch(const char *url, const char *method, const char *body);
void         fetch_free(FetchResult *r);

#endif // FETCH_H
