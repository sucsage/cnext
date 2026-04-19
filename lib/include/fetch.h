#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>

// =====================================================================
// cxn_fetch — HTTP client (libcurl wrapper) + in-RAM cache + JSON parse
//
// APIs:
//   1) cxn_fetch              — blocking (use from action handlers only)
//   2) cxn_fetch_async        — fire-and-forget, any method, optional cb
//   3) cxn_fetch_cached       — GET only, non-blocking, cached raw body
//   4) cxn_fetch_cached_typed — GET only, non-blocking, JSON → user struct
//
// Event-loop workers (page render) ต้องใช้ cached/async เท่านั้น —
// ห้ามเรียก cxn_fetch ตรง ๆ เพราะจะ block io_uring worker.
// =====================================================================

typedef struct {
    char  *body;    // null-terminated
    size_t len;
    int    status;  // HTTP status code
} FetchResult;

// ---------------------------------------------------------------------
// Blocking
// ---------------------------------------------------------------------
// method: "GET" | "POST" | "PUT" | "PATCH" | "DELETE" (or any custom)
// body:   request body for POST/PUT/PATCH — NULL สำหรับ GET/DELETE
// คืน NULL ถ้า network error / OOM
FetchResult *cxn_fetch(const char *url, const char *method, const char *body);
void         fetch_free(FetchResult *r);

// ---------------------------------------------------------------------
// Async fire-and-forget — ส่งงานเข้า thread pool, คืนทันที
// ---------------------------------------------------------------------
// method/body เหมือน cxn_fetch
// cb (optional) ถูกเรียกใน worker thread เมื่อ fetch เสร็จ — r อาจ NULL
// *** อย่าเข้าถึง PageCtx หรือ socket ของ request ต้นทาง จาก cb ***
typedef void (*FetchCallback)(FetchResult *r, void *user);
void cxn_fetch_async(const char *url, const char *method, const char *body,
                     FetchCallback cb, void *user);

// ---------------------------------------------------------------------
// Cached GET (raw body) — non-blocking
// ---------------------------------------------------------------------
// Fresh hit  → คืน strdup(body)  (caller free)
// Stale hit  → คืน strdup(body) + spawn bg refresh (stale-while-revalidate)
// Miss       → คืน NULL + spawn bg fetch; request ถัดไปจะได้ data
// ttl_seconds = อายุที่ถือว่า "fresh" — หลังจากนั้นยัง serve ได้อีก 2×ttl
// เป็น stale (bg refresh ระหว่างนั้น) ก่อน evict จริง
char *cxn_fetch_cached(const char *url, int ttl_seconds);

// ---------------------------------------------------------------------
// Cached typed GET — fetch + parse JSON → struct, cache binary struct
// ---------------------------------------------------------------------
// เก็บ struct ที่ parse แล้วใน cache → hit path = memcpy (ไม่ re-parse)
// คืน 1 = hit/success (out ถูกเติม), 0 = miss (bg fetch started)
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
    size_t         cap;       // string cap (sizeof field); 0 for non-string
} FetchField;

int cxn_fetch_cached_typed(const char *url, int ttl_seconds,
                           void *out, size_t out_size,
                           const FetchField *fields, size_t nfields);

// ---------------------------------------------------------------------
// JSON parse (standalone — for when you already have a body string)
// ---------------------------------------------------------------------
// Parse flat JSON object at top level, extract declared fields.
// Nested objects/arrays are skipped. Returns # fields populated.
int cxn_json_parse(const char *body, size_t len, void *out,
                   const FetchField *fields, size_t nfields);

// ---------------------------------------------------------------------
// Lifecycle (auto-init on first use — usually no need to call)
// ---------------------------------------------------------------------
void fetch_init(void);

#endif // FETCH_H
