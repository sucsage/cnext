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

#define BUFFER_SIZE      8192
#define WRITE_BUF_SIZE   8192
#define RING_ENTRIES     4096
#define CONN_POOL_SIZE   4096
#define ROUTE_TABLE_SIZE 256
#define STATIC_RESP_SIZE 512

#define UD_IGNORE  UINT64_C(0)
#define UD_ACCEPT  UINT64_C(1)

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
typedef enum { CONN_RECV, CONN_SEND } ConnState;

typedef struct Conn {
    int        fd;
    int        state;          // ConnState
    int        keep_alive;
    int        _pad0;          // explicit padding → next field 8-byte aligned
    size_t     rlen;
    size_t     search_from;
    size_t     wlen;
    size_t     wsent;
    // 48 bytes used — pad to 64 so rbuf starts at cache-line boundary
    char       _pad1[16];
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
static __thread char   tls_wbuf[WRITE_BUF_SIZE];
static __thread size_t tls_wlen;
static __thread int    tls_keep_alive;

// [1] Fixed Buffers — kernel keeps pool_buf pinned, ไม่ต้อง map/unmap ต่อ operation
static __thread int   use_fixed_buf   = 0;
// [3] Fixed Files — kernel keeps fd table, ไม่ต้อง lookup ต่อ operation
static __thread int   use_fixed_files = 0;
static __thread int  *fd_table;

static int worker_count = 0;

// =====================================================================
// [2] Route Hash Map (FNV-1a + open addressing)
// =====================================================================
typedef struct { char method[10]; char path[255]; ApiHandler handler; int used; } RouteSlot;
static RouteSlot route_table[ROUTE_TABLE_SIZE];

static uint32_t route_hash(const char *m, const char *p) {
    uint32_t h = 2166136261u;
    for (; *m; m++) { h ^= (uint8_t)*m; h *= 16777619u; }
    h ^= ' ';                h *= 16777619u;
    for (; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    return h & (ROUTE_TABLE_SIZE - 1);
}

static ApiHandler find_handler(const char *method, const char *path) {
    uint32_t idx = route_hash(method, path);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        RouteSlot *s = &route_table[(idx + i) & (ROUTE_TABLE_SIZE - 1)];
        if (!s->used) return NULL;
        if (strcmp(s->method, method) == 0 && strcmp(s->path, path) == 0)
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
    memset(route_table, 0, sizeof(route_table));
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
    size_t body_len = strlen(body);
    int n = snprintf(tls_wbuf, WRITE_BUF_SIZE,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: %s\r\n\r\n%s",
        status_code, status_text, content_type, body_len,
        tls_keep_alive ? "keep-alive" : "close", body);
    tls_wlen = (n > 0 && (size_t)n < WRITE_BUF_SIZE) ? (size_t)n : WRITE_BUF_SIZE - 1;
}

static void dispatch(HttpRequest *req, int fd) {
    ApiHandler h = find_handler(req->method, req->path);
    if (h) { h(req, fd); return; }
    if (!req->method[0]) return;
    StaticResp *r = &resp_404[tls_keep_alive];   // pre-built, no snprintf
    memcpy(tls_wbuf, r->buf, r->len);
    tls_wlen = r->len;
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

    // [1] Fixed Buffer: io_uring_prep_read_fixed ใช้ pre-pinned buffer โดยตรง
    // buf_index=0 → ชี้ไปที่ pool_buf ที่ register_buffers ไว้
    if (use_fixed_buf)
        io_uring_prep_read_fixed(sqe, fd, c->rbuf + c->rlen,
                                 BUFFER_SIZE - 1 - c->rlen, 0, 0);
    else
        io_uring_prep_recv(sqe, fd, c->rbuf + c->rlen,
                           BUFFER_SIZE - 1 - c->rlen, 0);

    if (use_fixed_files) sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, c);
}

static void submit_send(struct io_uring *ring, Conn *c) {
    c->state = CONN_SEND;
    struct io_uring_sqe *sqe = get_sqe(ring);
    int fd = use_fixed_files ? conn_fidx(c) : c->fd;

    if (use_fixed_buf)
        io_uring_prep_write_fixed(sqe, fd, c->wbuf + c->wsent,
                                  c->wlen - c->wsent, 0, 0);
    else
        io_uring_prep_send(sqe, fd, c->wbuf + c->wsent,
                           c->wlen - c->wsent, MSG_NOSIGNAL);

    if (use_fixed_files) sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, c);
}

static void conn_close(struct io_uring *ring, Conn *c) {
    struct io_uring_sqe *sqe = get_sqe(ring);
    if (use_fixed_files)
        // close_direct ปิด fd และลบออกจาก fixed file table อัตโนมัติ
        io_uring_prep_close_direct(sqe, conn_fidx(c));
    else
        io_uring_prep_close(sqe, c->fd);
    io_uring_sqe_set_data64(sqe, UD_IGNORE);
    pool_free(c);
}

// =====================================================================
// Process one CQE
// =====================================================================
static void process_cqe(struct io_uring *ring, struct io_uring_cqe *cqe) {
    uint64_t ud  = io_uring_cqe_get_data64(cqe);
    int      res = cqe->res;

    if (ud == UD_IGNORE) return;

    if (ud == UD_ACCEPT) {
        // [2] Multishot: resubmit เฉพาะตอนที่ kernel ยกเลิก multishot
        if (!(cqe->flags & IORING_CQE_F_MORE))
            submit_multishot_accept(ring);
        if (res < 0) return;

        // [8] TCP_NODELAY: ปิด Nagle algorithm สำหรับ latency ต่ำ
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

    Conn *c = (Conn *)(uintptr_t)ud;

    if (c->state == CONN_RECV) {
        if (res <= 0) { conn_close(ring, c); return; }
        c->rlen += (size_t)res;
        c->rbuf[c->rlen] = '\0';

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
            memcpy(c->wbuf, tls_wbuf, tls_wlen);
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

    // io_uring init — SQPOLL ก่อน, fallback ถ้าไม่มี CAP_SYS_NICE
    struct io_uring      ring;
    struct io_uring_params params = {
        .flags          = IORING_SETUP_SQPOLL,
        .sq_thread_idle = 2000,
    };
    if (io_uring_queue_init_params(RING_ENTRIES, &ring, &params) < 0) {
        fprintf(stderr, "[WARN] worker %d: SQPOLL unavailable → normal mode\n", wa.worker_id);
        if (io_uring_queue_init(RING_ENTRIES, &ring, 0) < 0) {
            fprintf(stderr,
                "[ERROR] io_uring_queue_init failed\n"
                "  Docker: เพิ่ม --security-opt seccomp=unconfined\n"
                "  kernel: ต้องการ Linux >= 5.1\n");
            exit(EXIT_FAILURE);
        }
    }

    // [1] Fixed Buffers — pin pool_buf ใน kernel, ไม่ต้อง map/unmap ต่อ operation
    // NOTE: ใช้ register_buffers เพื่อ pin memory เท่านั้น
    //       recv/send ยังใช้ regular path (read_fixed/write_fixed มีปัญหากับ socket)
    {
        struct iovec iov = { .iov_base = pool_buf,
                             .iov_len  = sizeof(Conn) * CONN_POOL_SIZE };
        if (io_uring_register_buffers(&ring, &iov, 1) == 0) {
            use_fixed_buf = 0; // pin memory แต่ไม่ใช้ fixed path สำหรับ socket
        }
    }

    // [3] Fixed Files — pre-register fd table, ไม่ต้อง fd lookup ต่อ operation
    {
        memset(fd_table, -1, sizeof(int) * CONN_POOL_SIZE);
        if (io_uring_register_files(&ring, fd_table, CONN_POOL_SIZE) == 0) {
            use_fixed_files = 0; // debug: ปิดชั่วคราว
        } else {
            fprintf(stderr, "[WARN] worker %d: fixed files unavailable\n", wa.worker_id);
        }
    }

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
