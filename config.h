#ifndef CNEXT_CONFIG_H
#define CNEXT_CONFIG_H

// =====================================================================
// cnext — compile-time tunables
// ---------------------------------------------------------------------
// Reference of every knob the framework exposes. Values are wrapped
// in #ifndef so a build can override any of them with `-DNAME=value`.
//
// These values are compiled into lib/libcnext.a. The shipped library
// was built using the defaults below; editing this file only affects
// a rebuild from source. Treat it as documentation of the defaults.
// =====================================================================

// ── server.c — io_uring + connection pool ────────────────────────────
#ifndef BUFFER_SIZE
#define BUFFER_SIZE      8192      // per-conn recv buffer
#endif
#ifndef WRITE_BUF_SIZE
#define WRITE_BUF_SIZE   8192      // per-conn send buffer (chunked sends)
#endif
#ifndef RESP_BUF_SIZE
#define RESP_BUF_SIZE    65536     // TLS response assembly buffer (64KB)
#endif
#ifndef RING_ENTRIES
#define RING_ENTRIES     4096      // io_uring SQ/CQ depth
#endif
#ifndef CONN_POOL_SIZE
#define CONN_POOL_SIZE   4096      // max concurrent connections
#endif
#ifndef ROUTE_TABLE_SIZE
#define ROUTE_TABLE_SIZE 256       // max registered routes
#endif
#ifndef STATIC_RESP_SIZE
#define STATIC_RESP_SIZE 512       // pre-built static response buffer
#endif
#ifndef ZC_THRESHOLD
#define ZC_THRESHOLD     4096      // zero-copy send for resp >= this (non-keepalive)
#endif
#ifndef BUF_RING_COUNT
#define BUF_RING_COUNT   4096      // provided-buffer ring; power of 2
#endif

// ── static.c — static file cache (public/) ───────────────────────────
#ifndef PUBLIC_DIR
#define PUBLIC_DIR       "public"
#endif
#ifndef MAX_FILES
#define MAX_FILES        64        // max cached static files
#endif
#ifndef MAX_FILE_SIZE
#define MAX_FILE_SIZE    (1024 * 1024)   // per-file cap (1MB)
#endif
#ifndef HDR_MAX
#define HDR_MAX          256       // static-response header buffer
#endif

// ── pages.c — SSR page rendering ─────────────────────────────────────
#ifndef PAGE_BUF
#define PAGE_BUF         (56 * 1024)   // per-worker page buffer (layout + page)
#endif
#ifndef MAX_PAGES
#define MAX_PAGES        64        // max registered .cxn / page.c handlers
#endif

// ── db/hot.c — in-RAM hash table ─────────────────────────────────────
#ifndef HOT_CAP
#define HOT_CAP          (1 << 13) // 8192 slots (keep as power of 2)
#endif
#ifndef KEY_MAX
#define KEY_MAX          320       // composite "ns\x01key" max length
#endif
#ifndef EVICT_INTERVAL_SEC
#define EVICT_INTERVAL_SEC 5       // background eviction period
#endif

// ── db/cold.c — LMDB async write queue ───────────────────────────────
#ifndef WQ_SIZE
#define WQ_SIZE          4096      // write queue depth; power of 2
#endif
#ifndef MAX_DBS
#define MAX_DBS          32        // max named MDB_dbi (namespaces)
#endif
#ifndef MAX_NS
#define MAX_NS           64        // max namespace-string length
#endif
#ifndef MAX_KEY
#define MAX_KEY          256       // max key length
#endif
#ifndef BATCH_MAX
#define BATCH_MAX        256       // commit every N jobs or when queue drains
#endif

// ── db/db.c — list cache ─────────────────────────────────────────────
#ifndef LIST_KEY_MAX
#define LIST_KEY_MAX     192
#endif

#endif // CNEXT_CONFIG_H
