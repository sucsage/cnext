#ifndef H2_H
#define H2_H

#include <stddef.h>
#include <stdint.h>

// =====================================================================
// HTTP/2 — nghttp2-backed session (one per H2 connection)
//
// Design:
//   - io_uring handles all I/O (recv/send raw bytes)
//   - nghttp2 handles framing, HPACK, flow control, stream lifecycle
//   - Existing handlers (send_response / set_raw_response) work unchanged
//     via TLS dispatch context set before each stream dispatch
//
// Usage:
//   1. Detect H2_PREFACE in first recv bytes
//   2. h2_create() once per connection
//   3. h2_recv() for every recv completion
//   4. h2_want_write() / h2_sndbuf() / h2_sndlen() / h2_drain() to flush
//   5. h2_destroy() on conn_close
// =====================================================================

// H2 Prior Knowledge connection preface (RFC 9113 §3.4)
#define H2_PREFACE     "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define H2_PREFACE_LEN 24

// Connection protocol tag (stored in Conn.proto)
#define PROTO_UNKNOWN 0
#define PROTO_H1      1
#define PROTO_H2      2

// Opaque session type
typedef struct H2State H2State;

// Dispatch callback — called once per complete request (END_STREAM received)
// server.c implements this as a static function capturing TLS context
typedef void (*H2DispatchFn)(int32_t stream_id,
                              const char *method, const char *path,
                              const char *cookie,
                              const char *body,   size_t body_len,
                              H2State *h2);

// ── Lifecycle ──────────────────────────────────────────────────────────
H2State *h2_create(H2DispatchFn fn);
void     h2_destroy(H2State *h);

// ── I/O ───────────────────────────────────────────────────────────────
// Feed raw bytes received from the network.  Returns 0 on success, -1 on error.
int h2_recv(H2State *h, const char *data, size_t len);

// Pending send buffer (drain after copying to wbuf / submitting send SQE)
int    h2_want_write(H2State *h);
char  *h2_sndbuf(H2State *h);
size_t h2_sndlen(H2State *h);
void   h2_drain(H2State *h, size_t n);

// ── Response helpers ───────────────────────────────────────────────────
// Called from send_response() / set_raw_response() via TLS dispatch context.

// Send a structured response (status + content-type + body)
void h2_respond(H2State *h, int32_t stream_id, int status,
                const char *content_type, const char *body, size_t body_len);

// Parse an HTTP/1.1-formatted response buffer and submit as H2 frames.
// Used to transparently support set_raw_response (pre-built static pages).
void h2_respond_raw(H2State *h, int32_t stream_id,
                    const char *buf, size_t len);

#endif // H2_H
