#define _GNU_SOURCE
#include <nghttp2/nghttp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h2.h"

// =====================================================================
// Per-stream state
// =====================================================================
#define MAX_H2_STREAMS 128

typedef struct {
    int32_t id;
    char    method[16];
    char    path[512];
    char    body[4096];
    size_t  body_len;
    // Response body for streaming to nghttp2 data provider
    char   *resp_body;
    size_t  resp_pos;
    size_t  resp_len;
    int     used;
} H2Stream;

// =====================================================================
// Session state
// =====================================================================
struct H2State {
    nghttp2_session *session;
    H2Stream         streams[MAX_H2_STREAMS];
    H2DispatchFn     dispatch_fn;
    // Accumulated send buffer (send_callback appends here)
    // sndrdpos = read head → drain ไม่ต้อง memmove อีกต่อไป
    uint8_t         *sndbuf;
    size_t           sndrdpos;
    size_t           sndlen;
    size_t           sndcap;
};

// ── Stream helpers ────────────────────────────────────────────────────
static void stream_free(H2Stream *s);   // forward declaration

// HTTP/2 client stream IDs เป็น odd integers: 1, 3, 5, ...
// map ตรงสู่ slot array → O(1) lookup ทุก nghttp2 callback
static inline int stream_slot(int32_t id) {
    return (int)(((uint32_t)id >> 1) & (MAX_H2_STREAMS - 1));
}

static H2Stream *stream_find(H2State *h, int32_t id) {
    H2Stream *s = &h->streams[stream_slot(id)];
    return (s->used && s->id == id) ? s : NULL;
}

static H2Stream *stream_alloc(H2State *h, int32_t id) {
    H2Stream *s = &h->streams[stream_slot(id)];
    if (s->used) stream_free(s);   // evict (stream IDs wrap after 2^31)
    memset(s, 0, sizeof(H2Stream));
    s->id   = id;
    s->used = 1;
    return s;
}

static void stream_free(H2Stream *s) {
    free(s->resp_body);
    memset(s, 0, sizeof(H2Stream));
}

// =====================================================================
// nghttp2 callbacks
// =====================================================================

// Called by nghttp2_session_send() — accumulate in sndbuf, flush via io_uring later
static ssize_t cb_send(nghttp2_session *session,
                        const uint8_t *data, size_t length,
                        int flags, void *user_data) {
    (void)session; (void)flags;
    H2State *h = user_data;
    size_t used = h->sndlen - h->sndrdpos;
    if (h->sndrdpos > 0 && used + length <= h->sndcap) {
        // compact: slide unread bytes to front, reset read head
        memmove(h->sndbuf, h->sndbuf + h->sndrdpos, used);
        h->sndlen   = used;
        h->sndrdpos = 0;
    }
    if (h->sndlen + length > h->sndcap) {
        size_t nc = h->sndcap ? h->sndcap * 2 : 4096;
        while (nc < h->sndlen + length) nc *= 2;
        uint8_t *nb = realloc(h->sndbuf, nc);
        if (!nb) return NGHTTP2_ERR_CALLBACK_FAILURE;
        h->sndbuf = nb;
        h->sndcap = nc;
    }
    memcpy(h->sndbuf + h->sndlen, data, length);
    h->sndlen += length;
    return (ssize_t)length;
}

// Called once per new HEADERS frame — allocate stream slot
static int cb_begin_headers(nghttp2_session *session,
                             const nghttp2_frame *frame, void *user_data) {
    (void)session;
    H2State *h = user_data;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST)
        stream_alloc(h, frame->hd.stream_id);
    return 0;
}

// Called per header name/value — fill method/path
static int cb_header(nghttp2_session *session, const nghttp2_frame *frame,
                     const uint8_t *name,  size_t namelen,
                     const uint8_t *value, size_t valuelen,
                     uint8_t flags, void *user_data) {
    (void)session; (void)flags;
    H2State  *h = user_data;
    H2Stream *s = stream_find(h, frame->hd.stream_id);
    if (!s) return 0;

    if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
        size_t n = valuelen < 15 ? valuelen : 15;
        memcpy(s->method, value, n);
        s->method[n] = '\0';
    } else if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
        size_t n = valuelen < 511 ? valuelen : 511;
        memcpy(s->path, value, n);
        s->path[n] = '\0';
    }
    return 0;
}

// Called per DATA chunk — accumulate request body
static int cb_data_chunk(nghttp2_session *session, uint8_t flags,
                          int32_t stream_id, const uint8_t *data,
                          size_t len, void *user_data) {
    (void)session; (void)flags;
    H2State  *h = user_data;
    H2Stream *s = stream_find(h, stream_id);
    if (!s) return 0;
    size_t copy = len;
    if (s->body_len + copy > sizeof(s->body) - 1)
        copy = sizeof(s->body) - 1 - s->body_len;
    memcpy(s->body + s->body_len, data, copy);
    s->body_len += copy;
    return 0;
}

// Called after a complete frame — dispatch on END_STREAM
static int cb_frame_recv(nghttp2_session *session,
                          const nghttp2_frame *frame, void *user_data) {
    (void)session;
    H2State *h = user_data;
    int end = frame->hd.flags & NGHTTP2_FLAG_END_STREAM;
    if (!end) return 0;
    if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA)
        return 0;

    H2Stream *s = stream_find(h, frame->hd.stream_id);
    if (s && h->dispatch_fn) {
        s->body[s->body_len] = '\0';
        h->dispatch_fn(s->id, s->method, s->path, s->body, s->body_len, h);
    }
    return 0;
}

// Called when stream closes — free stream slot
static int cb_stream_close(nghttp2_session *session, int32_t stream_id,
                             uint32_t error_code, void *user_data) {
    (void)session; (void)error_code;
    H2State  *h = user_data;
    H2Stream *s = stream_find(h, stream_id);
    if (s) stream_free(s);
    return 0;
}

// Data provider: streams response body bytes into nghttp2
static ssize_t cb_body_read(nghttp2_session *session, int32_t stream_id,
                              uint8_t *buf, size_t length, uint32_t *data_flags,
                              nghttp2_data_source *source, void *user_data) {
    (void)session; (void)stream_id; (void)user_data;
    H2Stream *s = source->ptr;
    size_t avail = s->resp_len - s->resp_pos;
    size_t copy  = avail < length ? avail : length;
    memcpy(buf, s->resp_body + s->resp_pos, copy);
    s->resp_pos += copy;
    if (s->resp_pos >= s->resp_len) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return (ssize_t)copy;
}

// =====================================================================
// Public API
// =====================================================================

H2State *h2_create(H2DispatchFn fn) {
    H2State *h = calloc(1, sizeof(H2State));
    if (!h) return NULL;
    h->dispatch_fn = fn;

    nghttp2_session_callbacks *cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback(cbs,                     cb_send);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs,         cb_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cbs,                cb_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,       cb_data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,            cb_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs,          cb_stream_close);
    nghttp2_session_server_new(&h->session, cbs, h);
    nghttp2_session_callbacks_del(cbs);

    // Send server SETTINGS immediately (queued → flushed to sndbuf)
    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, MAX_H2_STREAMS },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,    65535           },
    };
    nghttp2_submit_settings(h->session, NGHTTP2_FLAG_NONE,
                            settings, sizeof(settings) / sizeof(settings[0]));
    nghttp2_session_send(h->session);   // → sndbuf
    return h;
}

void h2_destroy(H2State *h) {
    if (!h) return;
    for (int i = 0; i < MAX_H2_STREAMS; i++)
        if (h->streams[i].used) stream_free(&h->streams[i]);
    nghttp2_session_del(h->session);
    free(h->sndbuf);
    free(h);
}

int h2_recv(H2State *h, const char *data, size_t len) {
    ssize_t n = nghttp2_session_mem_recv(h->session,
                                          (const uint8_t *)data, len);
    if (n < 0) {
        fprintf(stderr, "[H2] recv error: %s\n", nghttp2_strerror((int)n));
        return -1;
    }
    nghttp2_session_send(h->session);   // flush SETTINGS-ACK, WINDOW_UPDATE, etc.
    return 0;
}

int    h2_want_write(H2State *h) { return h->sndlen > h->sndrdpos; }
char  *h2_sndbuf(H2State *h)    { return (char *)h->sndbuf + h->sndrdpos; }
size_t h2_sndlen(H2State *h)    { return h->sndlen - h->sndrdpos; }

void h2_drain(H2State *h, size_t n) {
    h->sndrdpos += n;
    if (h->sndrdpos >= h->sndlen) {
        h->sndrdpos = 0;
        h->sndlen   = 0;   // buffer ว่างทั้งหมด → reset ทั้งคู่
    }
}

// ── Response submit ───────────────────────────────────────────────────

void h2_respond(H2State *h, int32_t stream_id, int status,
                const char *content_type, const char *body, size_t body_len) {
    H2Stream *s = stream_find(h, stream_id);
    if (!s) return;

    char status_str[4];
    snprintf(status_str, sizeof(status_str), "%d", status);
    char len_str[24];
    snprintf(len_str, sizeof(len_str), "%zu", body_len);

    nghttp2_nv hdrs[] = {
        { (uint8_t *)":status",        (uint8_t *)status_str,
          7, strlen(status_str), NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)"content-type",   (uint8_t *)content_type,
          12, strlen(content_type), NGHTTP2_NV_FLAG_NONE },
        { (uint8_t *)"content-length", (uint8_t *)len_str,
          14, strlen(len_str), NGHTTP2_NV_FLAG_NONE },
    };

    if (body_len > 0) {
        s->resp_body = malloc(body_len);
        if (!s->resp_body) {
            nghttp2_submit_rst_stream(h->session, NGHTTP2_FLAG_NONE,
                                      stream_id, NGHTTP2_INTERNAL_ERROR);
            nghttp2_session_send(h->session);
            return;
        }
        memcpy(s->resp_body, body, body_len);
        s->resp_len = body_len;
        s->resp_pos = 0;

        nghttp2_data_provider dp = {
            .source.ptr    = s,
            .read_callback = cb_body_read,
        };
        nghttp2_submit_response(h->session, stream_id, hdrs, 3, &dp);
    } else {
        nghttp2_submit_response(h->session, stream_id, hdrs, 3, NULL);
    }
    nghttp2_session_send(h->session);   // → sndbuf
}

// Parse a pre-built HTTP/1.1 response and submit as H2 frames.
// Allows set_raw_response (static pages) to work transparently.
void h2_respond_raw(H2State *h, int32_t stream_id,
                    const char *buf, size_t len) {
    // Parse status code from "HTTP/1.1 NNN ..."
    int status = 200;
    if (len > 9 && memcmp(buf, "HTTP/1.1 ", 9) == 0)
        status = atoi(buf + 9);

    // Find Content-Type header value
    const char *ct = "text/html; charset=utf-8";
    char ct_buf[128];
    const char *ct_hdr = (const char *)memmem(buf, len, "Content-Type: ", 14);
    if (ct_hdr) {
        const char *ct_end = (const char *)memchr(ct_hdr + 14, '\r',
                                                   (size_t)(buf + len - ct_hdr - 14));
        if (ct_end) {
            size_t ctlen = (size_t)(ct_end - ct_hdr - 14);
            if (ctlen < sizeof(ct_buf)) {
                memcpy(ct_buf, ct_hdr + 14, ctlen);
                ct_buf[ctlen] = '\0';
                ct = ct_buf;
            }
        }
    }

    // Find body start (after first blank line)
    const char *body     = (const char *)memmem(buf, len, "\r\n\r\n", 4);
    size_t      body_len = 0;
    if (body) { body += 4; body_len = (size_t)(buf + len - body); }
    else       { body = ""; }

    h2_respond(h, stream_id, status, ct, body, body_len);
}
