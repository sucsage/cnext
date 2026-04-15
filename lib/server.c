#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>   // TCP_NODELAY
#include <pthread.h>
#include <sched.h>         // pthread_setaffinity_np
#include <liburing.h>
#include "server.h"
#include "h2.h"

#define BUFFER_SIZE      8192
#define WRITE_BUF_SIZE   8192   // per-conn send buffer (ส่งเป็น chunk ได้)
#define RESP_BUF_SIZE    65536  // TLS response assembly buffer (64KB)
#define RING_ENTRIES     4096
#define CONN_POOL_SIZE   4096
#define ROUTE_TABLE_SIZE 256
#define STATIC_RESP_SIZE 512
#define ZC_THRESHOLD     4096   // zero-copy send สำหรับ response >= 4KB (non-keep-alive)

#define UD_IGNORE  UINT64_C(0)
#define UD_ACCEPT  UINT64_C(1)

// [7] Linked SQEs + [8] Zero-copy — bit-tag ใน Conn pointer
// Conn align(64) → low 6 bits = 0 เสมอ → ปลอดภัย 100%
// bit 0 = linked send CQE
// bit 1 = ZC send CQE (completion + notification ใช้ tag เดียวกัน)
// NOTE: UD_ACCEPT=1 มี bit 0 set → ต้องตรวจ ud==UD_ACCEPT ก่อน UD_IS_LINKED_SEND เสมอ
#define UD_IS_LINKED_SEND(ud)  ((ud) & UINT64_C(1))
#define UD_IS_ZC_NOTIF(ud)     ((ud) & UINT64_C(2))
#define UD_LINKED_SEND(c)      ((uint64_t)(uintptr_t)(c) | UINT64_C(1))
#define UD_ZC_NOTIF(c)         ((uint64_t)(uintptr_t)(c) | UINT64_C(2))
#define UD_CONN(ud)            ((Conn *)(uintptr_t)((ud) & ~UINT64_C(3)))

// =====================================================================
// [4] Cache-line Aligned Conn Struct
//
// Layout:
//   offset  0–47 : hot metadata (fd, state, counters) → cache line 1
//   offset 48–63 : padding → metadata stays in 1 cache line
//   offset    64 : rbuf  (8192 bytes = 128 cache lines)
//   offset  8256 : wbuf  (8192 bytes = 128 cache lines)
//   Total: 16448 = 257 × 64  → each element in array is cache-line aligned
// =====================================================================
typedef enum { CONN_RECV, CONN_SEND, CONN_ZC_CLOSE } ConnState;

typedef struct Conn {
    int        fd;
    int        state;          // ConnState
    int        keep_alive;
    int        zc_pending;     // ZC send notification pending — defer pool_free
    size_t     rlen;
    size_t     search_from;
    size_t     wlen;
    size_t     wsent;
    // 48 bytes used above; these 16 bytes fill to 64 → rbuf on cache-line boundary
    int        proto;        // PROTO_UNKNOWN / PROTO_H1 / PROTO_H2
    int        _pad0;
    void      *h2;           // H2State* — NULL for H1
    char       rbuf[BUFFER_SIZE];
    char       wbuf[WRITE_BUF_SIZE];
} __attribute__((aligned(64))) Conn;

_Static_assert(sizeof(Conn) % 64 == 0, "Conn must be a multiple of 64 bytes");

// =====================================================================
// Thread-local state (zero sharing between workers)
// pool_buf/free_list/fd_table are heap-allocated per worker to avoid
// putting 67MB of TLS on the thread stack (CONN_POOL_SIZE*sizeof(Conn)=~67MB)
// =====================================================================
static __thread Conn  *pool_buf;
static __thread Conn **free_list;
static __thread int    free_count;
static __thread int    tls_server_fd;
static __thread char   tls_resp[RESP_BUF_SIZE]; // response assembly (64KB)
static __thread size_t tls_wlen;
static __thread int    tls_keep_alive;

// [1] Fixed Buffers — kernel keeps pool_buf pinned, ไม่ต้อง map/unmap ต่อ operation
// [H2] TLS dispatch context — set before each stream dispatch so send_response
// and set_raw_response transparently route to h2_respond / h2_respond_raw
static __thread int     tls_h2_active = 0;
static __thread void   *tls_h2_state  = NULL;
static __thread int32_t tls_h2_stream = 0;

static __thread int   use_fixed_buf   = 0;
// [3] Fixed Files — kernel keeps fd table, ไม่ต้อง lookup ต่อ operation
static __thread int   use_fixed_files = 0;
static __thread int  *fd_table;
// [6] Provided Buffers — kernel เลือก recv buffer เอง, ไม่ต้องผูกกับ connection
#define BUF_RING_COUNT 256   // power of 2, shared pool per worker
#define BUF_GID        1
static __thread struct io_uring_buf_ring *buf_ring          = NULL;
static __thread char                     *buf_pool          = NULL;
static __thread int                       use_buf_ring      = 0;
// [8] Zero-copy send — pool_buf ต้อง register แล้ว จึงใช้ send_zc_fixed ได้
static __thread int                       use_buf_registered = 0;

static int worker_count = 0;

// =====================================================================
// [2] Route Hash Map (FNV-1a + open addressing)
// =====================================================================
typedef struct { char method[10]; char path[255]; ApiHandler handler; int used; } RouteSlot;
static RouteSlot route_table[ROUTE_TABLE_SIZE];

// Static file fallback — called when no route matches a GET request
typedef void (*StaticFallback)(const char *path, int socket_fd);
static StaticFallback static_fallback = NULL;

void set_static_fallback(StaticFallback fn) { static_fallback = fn; }

static uint32_t route_hash(const char *m, const char *p) {
    uint32_t h = 2166136261u;
    for (; *m; m++) { h ^= (uint8_t)*m; h *= 16777619u; }
    h ^= ' ';                h *= 16777619u;
    for (; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    return h & (ROUTE_TABLE_SIZE - 1);
}

static ApiHandler find_handler(const char *method, const char *path) {
    // Pass 1: exact match
    uint32_t idx = route_hash(method, path);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        RouteSlot *s = &route_table[(idx + i) & (ROUTE_TABLE_SIZE - 1)];
        if (!s->used) break;
        if (strcmp(s->method, method) == 0 && strcmp(s->path, path) == 0)
            return s->handler;
    }
    // Pass 2: prefix match — "/api/users/123" matches route "/api/users"
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        RouteSlot *s = &route_table[i];
        if (!s->used) continue;
        if (strcmp(s->method, method) != 0) continue;
        size_t rlen = strlen(s->path);
        if (strncmp(path, s->path, rlen) == 0 && path[rlen] == '/')
            return s->handler;
    }
    return NULL;
}

// =====================================================================
// [3] Pre-built Static Responses
// =====================================================================
typedef struct { char buf[STATIC_RESP_SIZE]; size_t len; } StaticResp;
static StaticResp resp_404[2];

static void build_static_responses(void) {
    const char *body = "{\"error\": \"Route Not Found\"}\n";
    size_t blen = strlen(body);
    for (int ka = 0; ka <= 1; ka++) {
        int n = snprintf(resp_404[ka].buf, STATIC_RESP_SIZE,
            "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: %s\r\n\r\n%s",
            blen, ka ? "keep-alive" : "close", body);
        resp_404[ka].len = (n > 0) ? (size_t)n : 0;
    }
}

void register_routes(Route *routes, size_t count) {
    build_static_responses();
    for (size_t i = 0; i < count; i++) {
        uint32_t idx = route_hash(routes[i].method, routes[i].path);
        while (route_table[idx].used) idx = (idx + 1) & (ROUTE_TABLE_SIZE - 1);
        route_table[idx].used = 1;
        strncpy(route_table[idx].method, routes[i].method, 9);
        strncpy(route_table[idx].path,   routes[i].path,   254);
        route_table[idx].handler = routes[i].handler;
    }
}

// =====================================================================
// Connection Pool
// =====================================================================
static inline int conn_fidx(Conn *c) { return (int)(c - pool_buf); }

static void pool_init(void) {
    // Heap-allocate so we don't blow the thread stack (64MB of TLS)
    pool_buf  = aligned_alloc(64, sizeof(Conn) * CONN_POOL_SIZE);
    free_list = malloc(sizeof(Conn *) * CONN_POOL_SIZE);
    fd_table  = malloc(sizeof(int) * CONN_POOL_SIZE);
    if (!pool_buf || !free_list || !fd_table) {
        fprintf(stderr, "[ERROR] pool_init: OOM\n"); exit(EXIT_FAILURE);
    }
    memset(pool_buf, 0, sizeof(Conn) * CONN_POOL_SIZE);
    free_count = CONN_POOL_SIZE;
    for (int i = 0; i < CONN_POOL_SIZE; i++) free_list[i] = &pool_buf[i];
}

static Conn *pool_alloc(struct io_uring *ring, int fd) {
    if (free_count == 0) return NULL;
    Conn *c = free_list[--free_count];
    memset(c, 0, sizeof(Conn));
    c->fd = fd; c->keep_alive = 1;
    // [3] Register real fd into fixed file table slot
    if (use_fixed_files)
        io_uring_register_files_update(ring, conn_fidx(c), &fd, 1);
    return c;
}

static void pool_free(Conn *c) { free_list[free_count++] = c; }

// =====================================================================
// Fast single-pass HTTP/1.1 parser
// =====================================================================
static int parse_request(char *buf, size_t len, HttpRequest *req, int *keep_alive) {
    memset(req, 0, sizeof(HttpRequest));
    char *p = buf, *end = buf + len;

    char *sp = memchr(p, ' ', end - p);
    if (!sp) return -1;
    size_t n = (size_t)(sp - p);
    if (!n || n >= sizeof(req->method)) return -2;
    memcpy(req->method, p, n); p = sp + 1;

    sp = memchr(p, ' ', end - p);
    if (!sp) return -1;
    n = (size_t)(sp - p);
    if (!n) return -2;
    if (n >= sizeof(req->path)) n = sizeof(req->path) - 1;
    memcpy(req->path, p, n); p = sp + 1;

    if (end - p < 10) return -1;
    *keep_alive = (p[7] == '1');
    p += 8;
    if (p < end && *p == '\r') p++;
    if (p < end && *p == '\n') p++;

    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') { p += 2; break; }
        char *crlf = NULL;
        for (char *q = p; q + 1 < end; q++)
            if (q[0] == '\r' && q[1] == '\n') { crlf = q; break; }
        if (!crlf) return -1;
        size_t llen = (size_t)(crlf - p);
        if (llen > 12 && strncasecmp(p, "User-Agent: ", 12) == 0) {
            size_t ulen = llen - 12;
            if (ulen >= sizeof(req->user_agent)) ulen = sizeof(req->user_agent) - 1;
            memcpy(req->user_agent, p + 12, ulen);
        } else if (llen > 12 && strncasecmp(p, "Connection: ", 12) == 0) {
            if (strncasecmp(p + 12, "close",      5)  == 0) *keep_alive = 0;
            if (strncasecmp(p + 12, "keep-alive", 10) == 0) *keep_alive = 1;
        }
        p = crlf + 2;
    }
    if (p < end) {
        n = (size_t)(end - p);
        if (n >= sizeof(req->body)) n = sizeof(req->body) - 1;
        memcpy(req->body, p, n);
    }
    return 0;
}

void send_response(int socket_fd, int status_code, const char *status_text,
                   const char *content_type, const char *body) {
    (void)socket_fd;

    // [H2] Route to nghttp2 when dispatching inside an H2 stream
    if (tls_h2_active) {
        h2_respond((H2State *)tls_h2_state, tls_h2_stream,
                   status_code, content_type, body, strlen(body));
        return;
    }

    size_t body_len = strlen(body);

    // เขียน headers ก่อน แล้ว memcpy body — หลีกเลี่ยง snprintf ที่ต้อง scan body อีกรอบ
    int hlen = snprintf(tls_resp, RESP_BUF_SIZE,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: %s\r\n\r\n",
        status_code, status_text, content_type, body_len,
        tls_keep_alive ? "keep-alive" : "close");

    if (hlen <= 0) { tls_wlen = 0; return; }

    size_t avail = RESP_BUF_SIZE - (size_t)hlen;
    size_t copy  = body_len < avail ? body_len : avail;
    memcpy(tls_resp + hlen, body, copy);
    tls_wlen = (size_t)hlen + copy;
}

void set_raw_response(const char *buf, size_t len) {
    // [H2] Pre-built HTTP/1.1 response → parse and submit as H2 frames
    if (tls_h2_active) {
        h2_respond_raw((H2State *)tls_h2_state, tls_h2_stream, buf, len);
        return;
    }
    size_t n = len < RESP_BUF_SIZE ? len : RESP_BUF_SIZE;
    memcpy(tls_resp, buf, n);
    tls_wlen = n;
}

static void dispatch(HttpRequest *req, int fd) {
    ApiHandler h = find_handler(req->method, req->path);
    if (h) { h(req, fd); return; }
    if (!req->method[0]) return;

    // Fallback: ลอง serve จาก public/ (CSS, JS, images ฯลฯ)
    if (static_fallback && strcmp(req->method, "GET") == 0) {
        static_fallback(req->path, fd);
        return;
    }

    StaticResp *r = &resp_404[tls_keep_alive];   // pre-built, no snprintf
    memcpy(tls_resp, r->buf, r->len);
    tls_wlen = r->len;
}

// =====================================================================
// [6] Provided Buffers setup
// =====================================================================
static void setup_buf_ring(struct io_uring *ring, int worker_id) {
    size_t ring_sz = (size_t)BUF_RING_COUNT * sizeof(struct io_uring_buf);
    buf_ring = aligned_alloc(4096, ring_sz);
    buf_pool = aligned_alloc(4096, (size_t)BUF_RING_COUNT * BUFFER_SIZE);
    if (!buf_ring || !buf_pool) goto fail;

    memset(buf_ring, 0, ring_sz);

    struct io_uring_buf_reg reg = {
        .ring_addr    = (unsigned long)buf_ring,
        .ring_entries = BUF_RING_COUNT,
        .bgid         = BUF_GID,
    };
    if (io_uring_register_buf_ring(ring, &reg, 0) != 0) {
        fprintf(stderr, "[WARN] worker %d: buf_ring unavailable\n", worker_id);
        goto fail;
    }

    for (int i = 0; i < BUF_RING_COUNT; i++) {
        io_uring_buf_ring_add(buf_ring,
                              buf_pool + (size_t)i * BUFFER_SIZE,
                              BUFFER_SIZE, i,
                              io_uring_buf_ring_mask(BUF_RING_COUNT), i);
    }
    io_uring_buf_ring_advance(buf_ring, BUF_RING_COUNT);
    use_buf_ring = 1;
    printf("[server] worker %d: provided buffers ready  count=%d\n",
           worker_id, BUF_RING_COUNT);
    return;
fail:
    free(buf_ring); buf_ring = NULL;
    free(buf_pool); buf_pool = NULL;
}

// =====================================================================
// io_uring Submit Helpers
// [1] Fixed Buffers + [3] Fixed Files applied here
// =====================================================================
static struct io_uring_sqe *get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) { io_uring_submit(ring); sqe = io_uring_get_sqe(ring); }
    return sqe;
}

// [2] Multishot Accept — 1 SQE รับได้เรื่อยๆ ไม่ต้อง resubmit ทุกครั้ง
static void submit_multishot_accept(struct io_uring *ring) {
    struct io_uring_sqe *sqe = get_sqe(ring);
    io_uring_prep_multishot_accept(sqe, tls_server_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, UD_ACCEPT);
}

static void submit_recv(struct io_uring *ring, Conn *c) {
    c->state = CONN_RECV;
    struct io_uring_sqe *sqe = get_sqe(ring);
    int fd = use_fixed_files ? conn_fidx(c) : c->fd;

    if (use_buf_ring) {
        // [6] Provided Buffers — addr=NULL, kernel เลือก buffer จาก ring
        io_uring_prep_recv(sqe, fd, NULL, BUFFER_SIZE, 0);
        sqe->flags    |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = BUF_GID;
    } else if (use_fixed_buf) {
        io_uring_prep_read_fixed(sqe, fd, c->rbuf + c->rlen,
                                 BUFFER_SIZE - 1 - c->rlen, 0, 0);
    } else {
        io_uring_prep_recv(sqe, fd, c->rbuf + c->rlen,
                           BUFFER_SIZE - 1 - c->rlen, 0);
    }

    if (use_fixed_files) sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, c);
}

// [H2] Dispatch callback — called by nghttp2 when END_STREAM arrives
// Runs synchronously inside h2_recv() → same thread → TLS context valid
static void h2_dispatch_fn(int32_t stream_id,
                            const char *method, const char *path,
                            const char *body,   size_t body_len,
                            H2State *h2) {
    tls_h2_active = 1;
    tls_h2_state  = h2;
    tls_h2_stream = stream_id;

    HttpRequest req;
    memset(&req, 0, sizeof(req));
    size_t n;
    n = strlen(method); if (n >= sizeof(req.method)) n = sizeof(req.method) - 1;
    memcpy(req.method, method, n);
    n = strlen(path);   if (n >= sizeof(req.path))   n = sizeof(req.path) - 1;
    memcpy(req.path, path, n);
    n = body_len;       if (n >= sizeof(req.body))   n = sizeof(req.body) - 1;
    memcpy(req.body, body, n);
    req.body[n] = '\0';

    dispatch(&req, -1);     // fd=-1: handlers use tls_h2_active path, fd is ignored
    tls_h2_active = 0;
}

static void submit_send(struct io_uring *ring, Conn *c);

// [H2] Helper: flush h2->sndbuf → c->wbuf → submit_send (returns 0 if nothing to send)
static int h2_flush_pending(struct io_uring *ring, Conn *c) {
    H2State *h = (H2State *)c->h2;
    if (!h2_want_write(h)) return 0;
    size_t copy = h2_sndlen(h) < WRITE_BUF_SIZE ? h2_sndlen(h) : WRITE_BUF_SIZE;
    memcpy(c->wbuf, h2_sndbuf(h), copy);
    h2_drain(h, copy);
    c->wlen = copy; c->wsent = 0;
    submit_send(ring, c);
    return 1;
}

static void submit_send(struct io_uring *ring, Conn *c) {
    c->state = CONN_SEND;
    struct io_uring_sqe *sqe = get_sqe(ring);
    int    fd      = use_fixed_files ? conn_fidx(c) : c->fd;
    size_t to_send = c->wlen - c->wsent;
    int    first   = (c->wsent == 0);

#ifdef IORING_OP_SEND_ZC
    // [8] Zero-copy send — non-keep-alive + first send + response >=4KB + registered buffer
    // wbuf อยู่ใน pool_buf ที่ register (buf_index=0) → kernel DMA โดยตรง ไม่ต้อง copy
    // ใช้เฉพาะ non-keep-alive: หลัง send จะ close เลย ไม่มีปัญหา wbuf ถูก reuse
    if (!c->keep_alive && !c->h2 && first && to_send >= ZC_THRESHOLD && use_buf_registered) {
        io_uring_prep_send_zc_fixed(sqe, fd, c->wbuf, to_send, MSG_NOSIGNAL, 0, 0);
        if (use_fixed_files) sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(sqe, UD_ZC_NOTIF(c));
        c->zc_pending = 1;
        return;
    }
#endif

    if (use_fixed_buf)
        io_uring_prep_write_fixed(sqe, fd, c->wbuf + c->wsent, to_send, 0, 0);
    else
        io_uring_prep_send(sqe, fd, c->wbuf + c->wsent, to_send, MSG_NOSIGNAL);

    if (use_fixed_files) sqe->flags |= IOSQE_FIXED_FILE;

    // [7] Linked SQEs — keep-alive + first send เท่านั้น
    // chain: send|LINK → recv  kernel ต่อ op เองโดยไม่ต้องกลับ userspace
    // ถ้า send partial: linked recv ถูก cancel (-ECANCELED) → handle ใน process_cqe
    if (c->keep_alive && first) {
        sqe->flags |= IOSQE_IO_LINK;
        io_uring_sqe_set_data64(sqe, UD_LINKED_SEND(c));

        // submit recv ที่ link ต่อจาก send
        struct io_uring_sqe *rqe = get_sqe(ring);
        c->state = CONN_RECV;
        c->rlen  = 0; c->search_from = 0;

        if (use_buf_ring) {
            io_uring_prep_recv(rqe, fd, NULL, BUFFER_SIZE, 0);
            rqe->flags    |= IOSQE_BUFFER_SELECT;
            rqe->buf_group = BUF_GID;
        } else {
            io_uring_prep_recv(rqe, fd, c->rbuf, BUFFER_SIZE - 1, 0);
        }
        if (use_fixed_files) rqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data(rqe, c);
    } else {
        // partial re-send หรือ non-keep-alive → ใช้ path เดิม
        io_uring_sqe_set_data(sqe, c);
    }
}

static void conn_close(struct io_uring *ring, Conn *c) {
    // [H2] Destroy session and all streams before freeing the Conn
    if (c->h2) { h2_destroy((H2State *)c->h2); c->h2 = NULL; }

    struct io_uring_sqe *sqe = get_sqe(ring);
    if (use_fixed_files)
        // close_direct ปิด fd และลบออกจาก fixed file table อัตโนมัติ
        io_uring_prep_close_direct(sqe, conn_fidx(c));
    else
        io_uring_prep_close(sqe, c->fd);
    io_uring_sqe_set_data64(sqe, UD_IGNORE);
    if (c->zc_pending)
        c->state = CONN_ZC_CLOSE; // [8] รอ ZC notification ก่อน pool_free
    else
        pool_free(c);
}

// =====================================================================
// Process one CQE
// =====================================================================
static void process_cqe(struct io_uring *ring, struct io_uring_cqe *cqe) {
    uint64_t ud  = io_uring_cqe_get_data64(cqe);
    int      res = cqe->res;

    if (ud == UD_IGNORE) return;

    // ตรวจ UD_ACCEPT ก่อน — UD_ACCEPT=1 มี bit 0 set เหมือน UD_IS_LINKED_SEND
    if (ud == UD_ACCEPT) {
        // [2] Multishot: resubmit เฉพาะตอนที่ kernel ยกเลิก multishot
        if (!(cqe->flags & IORING_CQE_F_MORE))
            submit_multishot_accept(ring);
        if (res < 0) return;

        // TCP_NODELAY: ปิด Nagle algorithm สำหรับ latency ต่ำ
        int one = 1;
        setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        Conn *c = pool_alloc(ring, res);
        if (!c) {
            struct io_uring_sqe *sqe = get_sqe(ring);
            io_uring_prep_close(sqe, res);
            io_uring_sqe_set_data64(sqe, UD_IGNORE);
            return;
        }
        submit_recv(ring, c);
        return;
    }

#ifdef IORING_OP_SEND_ZC
    // [8] Zero-copy send — bit 1 set
    // CQE แรก = completion (data ออกไปแล้ว), CQE สอง = notification (kernel ปล่อย buffer)
    if (UD_IS_ZC_NOTIF(ud)) {
        Conn *c = UD_CONN(ud);
        if (cqe->flags & IORING_CQE_F_NOTIF) {
            // Notification: kernel ปล่อย buffer แล้ว — คืน Conn ถ้า close deferred
            c->zc_pending = 0;
            if (c->state == CONN_ZC_CLOSE) pool_free(c);
            return;
        }
        // Completion CQE: data ถูกส่งแล้ว (kernel อาจ hold buffer อยู่ถ้า F_MORE set)
        if (!(cqe->flags & IORING_CQE_F_MORE)) c->zc_pending = 0;
        if (res < 0) { conn_close(ring, c); return; }
        c->wsent += (size_t)res;
        if (c->wsent < c->wlen) {
            // Partial ZC — ส่งที่เหลือด้วย regular send (wsent>0 → ไม่ใช้ ZC อีก)
            c->state = CONN_SEND;
            submit_send(ring, c);
            return;
        }
        c->wlen = c->wsent = 0;
        conn_close(ring, c); // non-keep-alive; deferred ถ้า zc_pending ยังค้างอยู่
        return;
    }
#endif

    // [7] Linked send CQE (bit 0 set)
    if (UD_IS_LINKED_SEND(ud)) {
        Conn *c = UD_CONN(ud);
        if (res >= 0 && (size_t)res == c->wlen) {
            // Full send สำเร็จ — linked recv กำลัง run อยู่แล้ว ไม่ต้องทำอะไร
            c->wlen = c->wsent = 0;
        } else if (res < 0) {
            // Send error — linked recv จะ cancel เอง
            c->state = CONN_SEND;   // ทำให้ -ECANCELED recv CQE ถูก ignore
            conn_close(ring, c);
        } else {
            // Partial send — linked recv ถูก cancel (-ECANCELED)
            // resubmit send ปกติ (ไม่ link) แล้ว recv จะตาม
            c->wsent += (size_t)res;
            c->state  = CONN_SEND;  // ทำให้ -ECANCELED recv CQE ถูก ignore
            submit_send(ring, c);   // wsent>0 → ไม่ link อีก
        }
        return;
    }

    Conn *c = UD_CONN(ud);

    // [7] -ECANCELED = linked recv ที่ถูก cancel เพราะ send partial/error
    //     c->state == CONN_SEND หมายความว่าเราจัดการแล้วใน linked send CQE
    if (res == -ECANCELED) return;

    if (c->state == CONN_RECV) {
        if (res <= 0) {
            // คืน buffer กลับ ring ก่อน close (ถ้า kernel เลือก buffer ให้แล้ว)
            if (use_buf_ring && (cqe->flags & IORING_CQE_F_BUFFER)) {
                int   bid  = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                char *pbuf = buf_pool + (size_t)bid * BUFFER_SIZE;
                io_uring_buf_ring_add(buf_ring, pbuf, BUFFER_SIZE, bid,
                                      io_uring_buf_ring_mask(BUF_RING_COUNT), 0);
                io_uring_buf_ring_advance(buf_ring, 1);
            }
            conn_close(ring, c); return;
        }

        if (use_buf_ring) {
            // [6] copy จาก provided buffer → c->rbuf แล้วคืน buffer ทันที
            int   bid  = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            char *pbuf = buf_pool + (size_t)bid * BUFFER_SIZE;
            size_t copy = (size_t)res;
            if (c->rlen + copy > BUFFER_SIZE - 1) copy = BUFFER_SIZE - 1 - c->rlen;
            memcpy(c->rbuf + c->rlen, pbuf, copy);
            c->rlen += copy;
            io_uring_buf_ring_add(buf_ring, pbuf, BUFFER_SIZE, bid,
                                  io_uring_buf_ring_mask(BUF_RING_COUNT), 0);
            io_uring_buf_ring_advance(buf_ring, 1);
        } else {
            c->rlen += (size_t)res;
        }
        c->rbuf[c->rlen] = '\0';

        // ── [H2] Protocol detection ────────────────────────────────────
        if (c->proto == PROTO_UNKNOWN) {
            // Partial-match guard: wait until we have enough bytes to compare
            size_t cmp = c->rlen < H2_PREFACE_LEN ? c->rlen : H2_PREFACE_LEN;
            if (memcmp(c->rbuf, H2_PREFACE, cmp) == 0) {
                if (c->rlen < H2_PREFACE_LEN) {
                    submit_recv(ring, c); return;   // accumulate full preface
                }
                c->proto = PROTO_H2;
                c->h2    = h2_create(h2_dispatch_fn);
                if (!c->h2) { conn_close(ring, c); return; }
            } else {
                c->proto = PROTO_H1;
            }
        }

        // ── [H2] Feed all buffered bytes to nghttp2 ───────────────────
        if (c->proto == PROTO_H2) {
            tls_keep_alive = 0; tls_wlen = 0;
            if (h2_recv((H2State *)c->h2, c->rbuf, c->rlen) < 0) {
                conn_close(ring, c); return;
            }
            c->rlen = 0; c->search_from = 0;
            // Flush any frames nghttp2 wants to send (SETTINGS-ACK, responses, ...)
            if (!h2_flush_pending(ring, c))
                submit_recv(ring, c);   // nothing to send yet — keep reading
            return;
        }
        // ── [H1] Parse + dispatch ──────────────────────────────────────

        size_t start = c->search_from > 3 ? c->search_from - 3 : 0;
        void  *found = memmem(c->rbuf + start, c->rlen - start, "\r\n\r\n", 4);
        c->search_from = c->rlen;

        if (!found) {
            if (c->rlen >= BUFFER_SIZE - 1) { conn_close(ring, c); return; }
            submit_recv(ring, c);
            return;
        }

        HttpRequest req;
        int ka = 1;
        if (parse_request(c->rbuf, c->rlen, &req, &ka) < 0) {
            conn_close(ring, c); return;
        }
        c->keep_alive = ka; c->rlen = 0; c->search_from = 0;
        tls_keep_alive = ka; tls_wlen = 0;
        dispatch(&req, c->fd);

        if (tls_wlen > 0) {
            memcpy(c->wbuf, tls_resp, tls_wlen);
            c->wlen = tls_wlen; c->wsent = 0;
        }
        submit_send(ring, c);
        return;
    }

    // CONN_SEND
    if (res < 0) { conn_close(ring, c); return; }
    c->wsent += (size_t)res;
    if (c->wsent < c->wlen) { submit_send(ring, c); return; }
    c->wlen = c->wsent = 0;

    // [H2] After send: flush more pending sndbuf chunks, or go back to recv
    if (c->proto == PROTO_H2) {
        if (!h2_flush_pending(ring, c))
            submit_recv(ring, c);   // sndbuf empty — read next client frame
        return;
    }

    // [H1]
    if (c->keep_alive) submit_recv(ring, c);
    else               conn_close(ring, c);
}

// =====================================================================
// Worker Thread
// =====================================================================
typedef struct { int server_fd; int worker_id; } WorkerArg;

static void *worker_thread(void *arg) {
    WorkerArg wa = *(WorkerArg *)arg;
    free(arg);
    tls_server_fd = wa.server_fd;

    // [5] CPU Affinity — pin worker ไว้กับ core เฉพาะ, ลด cache miss
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(wa.worker_id % worker_count, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
        fprintf(stderr, "[WARN] worker %d: setaffinity failed\n", wa.worker_id);

    pool_init();

    // io_uring init
    // Path A: SQPOLL + SINGLE_ISSUER — kernel poll thread + ลด internal lock
    //         (แต่ละ worker มี ring ของตัวเอง → SINGLE_ISSUER ถูกต้อง 100%)
    // Path B: SINGLE_ISSUER + COOP_TASKRUN + DEFER_TASKRUN
    //         batch completions ก่อน process — ลด context switch
    //         (DEFER_TASKRUN ต้องการ Linux >= 6.1, incompatible กับ SQPOLL)
    struct io_uring        ring;
    struct io_uring_params params = {
        .flags          = IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER,
        .sq_thread_idle = 2000,
    };
    if (io_uring_queue_init_params(RING_ENTRIES, &ring, &params) < 0) {
        fprintf(stderr, "[WARN] worker %d: SQPOLL unavailable → normal mode\n", wa.worker_id);
        // Path B: ไม่มี SQPOLL — ใช้ DEFER_TASKRUN แทน (Linux 6.1+)
        struct io_uring_params p2 = {
            .flags = IORING_SETUP_SINGLE_ISSUER |
                     IORING_SETUP_COOP_TASKRUN  |
                     IORING_SETUP_DEFER_TASKRUN,
        };
        if (io_uring_queue_init_params(RING_ENTRIES, &ring, &p2) < 0) {
            // Path C: plain fallback
            if (io_uring_queue_init(RING_ENTRIES, &ring, 0) < 0) {
                fprintf(stderr,
                    "[ERROR] io_uring_queue_init failed\n"
                    "  Docker: เพิ่ม --security-opt seccomp=unconfined\n"
                    "  kernel: ต้องการ Linux >= 5.1\n");
                exit(EXIT_FAILURE);
            }
        }
    }

    // [1] Fixed Buffers — pin pool_buf ใน kernel, ไม่ต้อง map/unmap ต่อ operation
    // NOTE: ใช้ register_buffers เพื่อ pin memory เท่านั้น
    //       recv/send ยังใช้ regular path (read_fixed/write_fixed มีปัญหากับ socket)
    {
        struct iovec iov = { .iov_base = pool_buf,
                             .iov_len  = sizeof(Conn) * CONN_POOL_SIZE };
        if (io_uring_register_buffers(&ring, &iov, 1) == 0) {
            use_fixed_buf      = 0; // pin memory แต่ไม่ใช้ fixed path สำหรับ socket
            use_buf_registered = 1; // ทำให้ใช้ send_zc_fixed ได้
        }
    }

    // [3] Fixed Files — pre-register fd table, ไม่ต้อง fd lookup ต่อ operation
    {
        memset(fd_table, -1, sizeof(int) * CONN_POOL_SIZE);
        if (io_uring_register_files(&ring, fd_table, CONN_POOL_SIZE) == 0) {
            use_fixed_files = 1; // kernel keeps fd table — ไม่ต้อง lookup ต่อ operation
        } else {
            fprintf(stderr, "[WARN] worker %d: fixed files unavailable\n", wa.worker_id);
        }
    }

    // [6] Provided Buffers — shared recv pool per worker (Linux 5.19+)
    setup_buf_ring(&ring, wa.worker_id);

    // [2] Multishot Accept — 1 SQE รับได้เรื่อยๆ
    submit_multishot_accept(&ring);

    for (;;) {
        io_uring_submit_and_wait(&ring, 1);

        struct io_uring_cqe *cqe;
        unsigned head, count = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            process_cqe(&ring, cqe);
            count++;
        }
        io_uring_cq_advance(&ring, count);
    }

    io_uring_queue_exit(&ring);
    return NULL;
}

// =====================================================================
// Server Setup
// =====================================================================
static int create_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(port)
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(EXIT_FAILURE); }
    if (listen(fd, SOMAXCONN) < 0) { perror("listen"); exit(EXIT_FAILURE); }
    return fd;
}

int setup_server(int port) {
    // [4] Dynamic Worker Count
    worker_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (worker_count <= 0) worker_count = 4;
    return create_server_socket(port);
}

void accept_clients(int server_fd, int port) {
    printf("C Edge Server  port=%d  workers=%d  "
           "(io_uring+SQPOLL · multishot-accept · fixed-buf · fixed-files · "
           "SO_REUSEPORT · keep-alive · TCP_NODELAY)\n",
           port, worker_count);

    for (int i = 0; i < worker_count; i++) {
        WorkerArg *wa = malloc(sizeof(WorkerArg));
        wa->server_fd = (i == 0) ? server_fd : create_server_socket(port);
        wa->worker_id = i;
        pthread_t t;
        pthread_create(&t, NULL, worker_thread, wa);
        pthread_detach(t);
    }
    for (;;) pause();
}
