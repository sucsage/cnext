#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>

// =====================================================================
// cxn_fetch — unified HTTP client (libcurl) + cache + JSON parse
// ---------------------------------------------------------------------
// One function, one options struct. Cache / wait / parse / async are
// orthogonal axes controlled by FetchOpts fields. Use C99 designated
// initializers so you only spell out the axes you need.
//
//   Blocking raw (action handler):
//     FetchResult *r = NULL;
//     cxn_fetch(url, &(FetchOpts){ .wait_ms = FETCH_BLOCK,
//                                  .out_result = &r });
//
//   Cached typed + wait on cold cache (Next.js-style SSR):
//     Todo t = {0};
//     int ok = cxn_fetch(url, &(FetchOpts){
//         .ttl       = 60,
//         .wait_ms   = 500,
//         .out       = &t, .out_size = sizeof(t),
//         .fields    = TODO_FIELDS, .nfields = TODO_NFIELDS,
//     });
//
//   Fire-and-forget POST:
//     cxn_fetch(url, &(FetchOpts){
//         .method = "POST", .body = json, .async = 1 });
// =====================================================================

typedef struct {
    char  *body;    // null-terminated
    size_t len;
    int    status;  // HTTP status code
} FetchResult;

void fetch_free(FetchResult *r);

// ---------------------------------------------------------------------
// JSON field descriptor — used by .out / .fields parsing
// ---------------------------------------------------------------------
typedef enum {
    F_STR,   // char[cap] in-struct
    F_INT,   // int
    F_I64,   // long long
    F_F64,   // double
    F_BOOL   // int (0/1)
} FetchFieldType;

typedef struct {
    const char    *key;       // JSON key to match
    FetchFieldType type;
    size_t         offset;    // offsetof(YourStruct, field)
    size_t         cap;       // string cap; 0 for non-string
} FetchField;

// ---------------------------------------------------------------------
// Async callback — runs on a worker thread; do NOT touch the originating
// request's PageCtx/socket from inside.
// ---------------------------------------------------------------------
typedef void (*FetchCallback)(FetchResult *r, void *user);

// ---------------------------------------------------------------------
// Wait mode sentinels
// ---------------------------------------------------------------------
enum {
    FETCH_NOWAIT = 0,      // miss → return 0 immediately; bg fetch starts
    FETCH_BLOCK  = -1,     // block until done (no timeout)
};

// ---------------------------------------------------------------------
// FetchOpts — every field defaults to zero (safe: GET, no cache, no wait)
// ---------------------------------------------------------------------
typedef struct {
    // Request
    const char *method;          // default "GET"
    const char *body;            // request body (POST/PUT/PATCH)

    // Caching — 0 = disabled. stale-while-revalidate: stale serves up to 2×ttl.
    int ttl;

    // Wait on cache miss — FETCH_NOWAIT | >0 ms | FETCH_BLOCK
    int wait_ms;

    // Fire-and-forget. Overrides wait_ms. cb (if set) runs in worker thread.
    int           async;
    FetchCallback cb;
    void         *user;

    // Output — any combination:
    //   out_result set → *out_result = malloc'd FetchResult (fetch_free after)
    //   out + out_size + fields + nfields set → parsed struct written to out
    FetchResult      **out_result;
    void              *out;
    size_t             out_size;
    const FetchField  *fields;
    size_t             nfields;
} FetchOpts;

// ---------------------------------------------------------------------
// The one function.
//
// Returns:
//   1 = data available (out / out_result populated)
//   0 = miss — no data this call (wait_ms expired, or async started)
//  -1 = error (network / parse / OOM)
// ---------------------------------------------------------------------
int cxn_fetch(const char *url, const FetchOpts *opts);

// ---------------------------------------------------------------------
// Standalone JSON parse (when you already have a body string)
// ---------------------------------------------------------------------
int cxn_json_parse(const char *body, size_t len, void *out,
                   const FetchField *fields, size_t nfields);

// ---------------------------------------------------------------------
// Lifecycle (auto-init on first use — usually no need to call)
// ---------------------------------------------------------------------
void fetch_init(void);

#endif // FETCH_H
