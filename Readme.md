# cnext

Next.js-style SSR framework written in C — file-based routing, `.cxn` templates, server actions, compiled to a single binary.

```bash
make dev        # live-reload dev server (Docker)
make start      # production build + run  (Docker)
make native     # Linux only — native build, no Docker
make help       # list all targets
```

---

## Stack

| Layer | Technology |
|-------|-----------|
| Async I/O | io_uring — multishot accept, provided buffers, SQPOLL, zero-copy send |
| HTTP | HTTP/1.1 keep-alive + HTTP/2 (nghttp2, HPACK, stream multiplexing) |
| Persistence | LMDB — async write queue, batch commit, `MDB_MAPASYNC` |
| Cache | RAM hash table — FNV-1a, LRU eviction, background eviction thread |
| Fetch | libcurl wrapper — non-blocking via background thread + cache |

---

## Install

`cnext` runs on **Linux** because it's built on `io_uring` (a Linux kernel API). On macOS and Windows you run it inside Docker.

### Docker (recommended — works on macOS / Windows / Linux)

Once Docker is installed, `make dev` is the only command you need.

- **macOS** — install [Docker Desktop for Mac](https://docs.docker.com/desktop/install/mac-install/) (Apple Silicon or Intel). Open Docker Desktop once after install so the daemon starts.
- **Windows** — install [Docker Desktop for Windows](https://docs.docker.com/desktop/install/windows-install/). It uses WSL 2 under the hood; the installer sets that up for you.
- **Linux** — install [Docker Engine](https://docs.docker.com/engine/install/), then add your user to the `docker` group so you don't need `sudo`:

  ```bash
  sudo usermod -aG docker $USER && newgrp docker
  ```

Verify: `docker run --rm hello-world`

### Native (Linux only, no Docker)

```bash
sudo apt install build-essential python3 \
                 liblmdb-dev libnghttp2-dev libcurl4-openssl-dev libssl-dev

# liburing 2.6+ — build from source if your distro's package is older
git clone --depth 1 --branch liburing-2.6 https://github.com/axboe/liburing.git
cd liburing && ./configure --prefix=/usr/local && make && sudo make install && sudo ldconfig

cd /path/to/cnext && make native && ./server
```

`make native` checks deps before building and prints exactly what's missing. On macOS / Windows it refuses to run and points you at `make dev`.

---

## File-based Routing

| File | Method | Route |
|------|--------|-------|
| `src/page.cxn` | GET | `/` |
| `src/blog/page.cxn` | GET | `/blog` |
| `src/path/route.c` | any | `/path` |
| `src/path/action.c` | POST | `/action/path/create` |
| `src/path/action_delete.c` | POST | `/action/path/delete` |
| `src/layout.cxn` | layout | wraps every page at `{{children}}` |

Routes are derived from the folder path — no `@route` needed. All handlers auto-register via `__attribute__((constructor))`, no manual wiring in `main.c`.

---

## .cxn Templates

Pages live in `.cxn` files — HTML mixed with C, compiled at build time by `tools/cxnc`.

```
<!-- src/users/page.cxn  →  GET /users  (route derived from path) -->
<%inc db/db.h %>

<%!
static void render_card(const char *key, const char *val, void *arg) {
    PageCtx *ctx = arg;
    page_writef(ctx, "<div class='card'>%s</div>", val);
}
%>

<section class='page'>
  <% db_scan("users", render_card, ctx); %>
</section>
```

### Directives

| Directive | Description |
|-----------|-------------|
| `<%inc file.h %>` | `#include "file.h"` |
| `<%inc <stdlib.h> %>` | `#include <stdlib.h>` |
| `@route /path` | Override the path-derived route (optional) |
| `@fn funcname` | Generate public `void funcname(PageCtx *ctx)` — no route |
| `@fn Name(args)` | Component — generate `void Name(PageCtx *ctx, args)` + matching `.h` |
| `@meta key "value"` | Per-page metadata (`title`, `description`, `og_image`, `canonical`) |
| `{{children}}` | (in `layout.cxn` only) slot where the page body renders |
| `<%! ... %>` | C code at file scope (close with `%>` on its own line) |
| `<% code; %>` | C code inside `render()` — single-line or multi-line (close with `%>` on its own line) |
| `{{expr}}` | `page_writef(ctx, "%s", expr)` — **string only**; use `<% page_writef(ctx, "%d", x); %>` for ints/floats |
| plain HTML | `page_write(ctx, "...\n")` |

### Layout

One `src/layout.cxn` wraps every page at the `{{children}}` marker — no explicit calls in pages.

```
<!DOCTYPE html>
<html lang="en">
<head>
  <title>cnext</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <nav>...</nav>
  <main>
    {{children}}
  </main>
</body>
</html>
```

If `layout.cxn` is absent, pages render standalone via a weak default in `lib/include/pages.h`.

### Components — `@fn Name(args)`

Reusable partials with typed props. Write a `.cxn` file with `@fn Name(args...)`; cxnc emits a `.c` definition **and** a `.h` forward declaration. Any page that `<%inc>`s the header can call the component with full gcc type-checking.

```
<!-- src/components/card.cxn -->
@fn Card(const char *title, const char *desc)

<div class="card">
  <h3>{{title}}</h3>
  <p>{{desc}}</p>
</div>
```

```
<!-- src/some/page.cxn -->
<%inc src/components/card.cxn %>

<% Card(ctx, "Hello", "This is a card"); %>
<% Card(ctx, "Next",  "Another one"); %>
```

Notes:
- Components live anywhere; `src/components/**/*.cxn` is a convention, not a requirement. They don't register a route.
- `ctx` is always the first argument — `cxnc` prepends `PageCtx *ctx` to the signature you write.
- For many fields, prefer a props struct: define `typedef struct { ... } CardProps;` in a `<%! %>` block and use `@fn Card(const CardProps *p)`. Call with a compound literal: `Card(ctx, &(CardProps){ .title="X", .desc="Y" });`.
- `<%inc foo/bar.cxn %>` is auto-rewritten to `#include "foo/bar_cxn.h"` — cxnc generates the header into `.cnext/` and the Makefile adds `-I.cnext` so the include resolves. Write `.cxn` paths at the call site; you never touch `_cxn.h` by hand.

### Per-page metadata

Declare `@meta` directives at the top of any `page.cxn`:

```
@meta title       "Blog — cnext"
@meta description "Latest posts"
@meta og_image    "/og/blog.png"
```

The layout picks them up by calling `page_emit_meta_head(ctx)` inside `<head>`:

```
<head>
  <meta charset="UTF-8">
<% page_emit_meta_head(ctx); %>
  <link rel="stylesheet" href="/style.css">
</head>
```

That call emits `<title>` (falling back to `cnext`), `<meta name="description">`, `<link rel="canonical">` and the matching `og:*` tags. Call `page_current_meta()` from custom layout code if you need direct access.

---

## HTTPS

Plaintext HTTP/1.1 + HTTP/2 (h2c) runs on port **8080**. TLS terminates on port **8443** using OpenSSL + kTLS.

Drop `cert.pem` + `key.pem` into a **`certs/`** folder at the project root — the framework loads them at runtime, no rebuild required.

```bash
make gen-cert            # one-time: writes certs/cert.pem + certs/key.pem (self-signed, 10y)
make dev                 # TLS listener auto-starts on 8443
curl -k https://localhost:8443/
```

Replace the dev cert with a real one by overwriting `certs/cert.pem` + `certs/key.pem`. Override the paths with env vars `CNEXT_TLS_CERT` / `CNEXT_TLS_KEY` if you prefer to keep certs elsewhere.

If no cert is present at startup, TLS is silently skipped — the plaintext listener on 8080 keeps running. The first cut terminates HTTP/1.1 only; HTTP/2-over-TLS is tracked as future work. Compile TLS out entirely with `-DTLS_ENABLED=0` if you want zero OpenSSL linkage.

---

## Server Actions

Create `src/path/action.c` — POST handler that redirects after processing:

```c
#include "route.h"
#include "server.h"
#include "db/db.h"
#include <string.h>

static void action(HttpRequest *req, int socket_fd) {
    char name[256] = "";
    parse_form(req->body, "name", name, sizeof(name));
    if (!name[0]) { send_redirect(socket_fd, "/path?error=empty"); return; }

    // ... db_put ...

    send_redirect(socket_fd, "/path");
}

REGISTER_ACTION("/action/path/create")
```

For multiple actions per directory use separate files (`action.c`, `action_delete.c`, …) since each defines `static void action`.

### Helpers

```c
void parse_form(const char *body, const char *key, char *out, size_t out_len);
void send_redirect(int socket_fd, const char *location);
```

---

## Database API

Two-tier storage: **hot** (RAM) and **cold** (LMDB on disk).

```c
#include "db/db.h"

db_put(DB_BOTH, "ns", "key", "value");  // hot + cold (recommended)
db_put(DB_HOT,  "ns", "key", "value");  // RAM only
db_put(DB_COLD, "ns", "key", "value");  // LMDB only, async

char *val  = db_get("ns", "key");   // hot first, then cold; caller must free()
db_del("ns", "key");                // both tiers
db_scan("ns", callback, ctx);       // iterate all (waits for pending writes)
char *list = db_list("ns");         // JSON "[v1,v2,...]"; caller must free()
```

Cold writes are async. `db_scan` / `db_list` call `cold_flush()` internally before reading.

---

## HTTP Fetch

One function, one options struct: `cxn_fetch(url, &opts)`. Cache, wait-on-miss, JSON parsing, and fire-and-forget are orthogonal fields on `FetchOpts`. Use C99 designated initializers so you only spell out the axes you need — every field defaults to a safe zero.

### Usage matrix

| Goal | Opts |
|---|---|
| Blocking raw (action handler) | `.wait_ms=FETCH_BLOCK, .out_result=&r` |
| Fire-and-forget POST | `.method="POST", .body=json, .async=1` |
| Cached raw (non-blocking) | `.ttl=60, .out_result=&r` |
| Cached typed (non-blocking, zero-parse hit) | `.ttl=60, .out=&t, .out_size=sizeof(t), .fields=F, .nfields=N` |
| **Cached typed + wait on cold cache** (Next.js SSR) | `.ttl=60, .wait_ms=500, .out=&t, ...` |
| Async with completion callback | `.async=1, .cb=my_cb, .user=ctx` |

### Return value

- `1` — data available (`out` / `out_result` populated)
- `0` — miss (wait_ms expired, or async started)
- `-1` — error

### Examples

Blocking (use from `action.c` — they block anyway while user waits for redirect):

```c
#include "fetch.h"

FetchResult *r = NULL;
cxn_fetch("https://api.example.com/data", &(FetchOpts){
    .wait_ms    = FETCH_BLOCK,
    .out_result = &r,
});
if (r) { /* r->body, r->len, r->status */ fetch_free(r); }
```

Cached typed with **wait-on-miss** — the first visitor to a cold cache pays ~100ms of upstream latency and gets populated HTML; subsequent visitors get sub-µs cache hits:

```c
<%inc fetch.h %>

<%!
typedef struct {
    int  userId;
    int  id;
    char title[256];
    int  completed;
} Todo;

static const FetchField TODO_FIELDS[] = {
    {"userId",    F_INT,  offsetof(Todo, userId),    0},
    {"id",        F_INT,  offsetof(Todo, id),        0},
    {"title",     F_STR,  offsetof(Todo, title),     sizeof(((Todo*)0)->title)},
    {"completed", F_BOOL, offsetof(Todo, completed), 0},
};
#define TODO_NFIELDS (sizeof(TODO_FIELDS)/sizeof(TODO_FIELDS[0]))
%>

<% Todo t = {0};
   int ok = cxn_fetch("https://jsonplaceholder.typicode.com/todos/1",
                      &(FetchOpts){
                          .ttl     = 60,
                          .wait_ms = 500,
                          .out     = &t, .out_size = sizeof(t),
                          .fields  = TODO_FIELDS, .nfields = TODO_NFIELDS,
                      });
%>

<div>
  <% if (ok) { %>
    <% page_writef(ctx, "<div>#%d — %s</div>", t.id, t.title); %>
    <div>{{ t.completed ? "done" : "pending" }}</div>
  <% } else { %>
    <div>loading...</div>
  <% } %>
</div>
```

Fire-and-forget POST (analytics, webhooks):

```c
cxn_fetch("https://analytics.example.com/event", &(FetchOpts){
    .method = "POST",
    .body   = "{\"event\":\"page_view\"}",
    .async  = 1,
});
```

### Opts fields

| Field | Purpose |
|---|---|
| `method` | `"GET"` (default), `"POST"`, `"PUT"`, `"PATCH"`, `"DELETE"`, or any custom |
| `body` | Request body (POST/PUT/PATCH); `NULL` otherwise |
| `ttl` | Cache window (sec); `0` disables cache. Stale-while-revalidate up to `2×ttl` |
| `wait_ms` | Behavior on cache miss: `FETCH_NOWAIT` (0), `>0` = block up to N ms, `FETCH_BLOCK` = block forever |
| `async` | Fire-and-forget — returns `0` immediately; `cb` (if set) runs in a worker thread |
| `cb` / `user` | Callback for async; **do not touch the originating request's `PageCtx`/socket** |
| `out_result` | `FetchResult**` — raw body + status; caller must `fetch_free` |
| `out` / `out_size` / `fields` / `nfields` | Parse JSON into a struct; cache stores the parsed binary so hit path is pure `memcpy` |

### Standalone JSON parse

If you already have a body string:

```c
Todo t = {0};
cxn_json_parse(body, len, &t, TODO_FIELDS, TODO_NFIELDS);
```

Field types: `F_STR` (char array in-struct), `F_INT`, `F_I64`, `F_F64`, `F_BOOL`. Nested JSON objects/arrays are skipped — declare only the top-level keys you need. One-pass tokenizer, zero heap allocation.

### Arrays — `[{...}, {...}]`

For endpoints that return a top-level JSON array, pass `out_array` + `out_count` instead of `out`. cxn_fetch parses each element into the same struct shape, mallocs a contiguous array, and caches the packed binary blob — hit path is still zero-reparse.

```c
Todo *todos = NULL;
size_t n = 0;
int ok = cxn_fetch("https://jsonplaceholder.typicode.com/todos", &(FetchOpts){
    .ttl       = 60,
    .wait_ms   = 500,
    .out_array = (void **)&todos,
    .out_count = &n,
    .max_elems = 200,                      // cap; defaults to 4096
    .fields    = TODO_FIELDS, .nfields = TODO_NFIELDS,
});
if (ok) {
    for (size_t i = 0; i < n; i++)
        page_writef(ctx, "<li>#%d — %s</li>", todos[i].id, todos[i].title);
    free(todos);
}
```

Standalone: `cxn_json_parse_array(body, len, fields, nfields, sizeof(T), max, &out, &count)`.

### Performance notes

- Internal thread pool (4 workers by default) + 1024-slot hash cache (FNV-1a, open-addressing)
- Cache stores the **parsed binary struct** for typed fetches → hit path = `memcpy`, no JSON re-parse
- Per-thread `CURL *` handle reuses TCP/TLS keep-alive across calls (saves ~50–200ms per request)
- In-flight dedup: 1000 concurrent requests for the same URL fire **one** upstream call
- Stale-while-revalidate: users never block on a refresh — they always get fresh or stale-served data while the bg worker refills the cache
- `wait_ms>0` uses a `pthread_cond_timedwait` on a global condvar broadcast by workers on cache store — no busy-polling

---

## Sessions

Cookie-backed server-side state. On first access, the framework allocates a 32-hex SID and pins it with a `Set-Cookie: cnext_sid=...; HttpOnly; SameSite=Lax; Max-Age=86400` response header. Values live in `db_hot` under the `sess:<sid>` namespace — single-process RAM, not shared across restarts.

```c
#include "session.h"

const char *sid = session_id(req);                    // read or allocate
char *name = session_get(req, "name");                // caller free()s
session_set(req, "name", "alice", 0);                 // 0 = default 1-day TTL
session_del(req, "name");
```

- `session_id` is stable within a request (TLS-cached on the cookie string) and across requests (cookie round-trip).
- `session_set` accepts a per-key TTL in seconds; `0` means default. Values evict on hot-cache pressure.
- For durable state, mirror to `db_cold` yourself — the session store is deliberately RAM-only.

Demo: [src/counter/page.cxn](src/counter/page.cxn) + [src/counter/fragment.cxn](src/counter/fragment.cxn) at `/counter`.

---

## Fragments — `fragment.cxn` + HTMX

A `fragment.cxn` renders **without** the root layout and exposes a public symbol so action handlers can re-render it directly. Pair with HTMX for reactive-lite swaps — no client bundler, no JSON API, just HTML over POST.

```
src/counter/
  page.cxn              → GET /counter      (full page, includes HTMX from CDN)
  fragment.cxn          → GET /counter/fragment
  action.c              → POST /action/counter/{inc,reset}
```

The fragment file:

```
<!-- src/counter/fragment.cxn -->
<%inc session.h %>
<%inc <stdlib.h> %>

<% char *cur = session_get(req, "count"); %>
<% int n = cur ? atoi(cur) : 0; free(cur); %>

<div id="counter">
  <% page_writef(ctx, "<p>count: <strong>%d</strong></p>", n); %>
  <button hx-post="/action/counter/inc"   hx-target="#counter" hx-swap="outerHTML">+1</button>
  <button hx-post="/action/counter/reset" hx-target="#counter" hx-swap="outerHTML">reset</button>
</div>
```

cxnc emits a matching header so the action can call the fragment directly:

```c
// src/counter/action.c
#include "src/counter/fragment_cxn.h"   // auto-generated header (from fragment.cxn)

static void action(HttpRequest *req, int socket_fd) {
    /* mutate session_set/session_del … */
    char buf[4096];
    PageCtx ctx = { .buf = buf, .len = 0, .cap = sizeof(buf) };
    cxn_fragment_counter_fragment(req, &ctx);
    ctx.buf[ctx.len] = '\0';
    send_response(socket_fd, 200, "OK", "text/html; charset=utf-8", ctx.buf);
}
```

Conventions:
- Generated symbol is `cxn_fragment_<route-slug>` (`/counter/fragment` → `cxn_fragment_counter_fragment`).
- Fragments skip `app_layout` — only the fragment's own HTML is sent. Hosting page includes `<script src="https://unpkg.com/htmx.org@1.9.12" defer></script>`.
- Action handlers return the fragment HTML (200, `text/html`); HTMX swaps it at `hx-target`.

---

## Dev Mode

Save any source file and the server rebuilds, restarts, and the browser auto-reloads.

```bash
make dev          # start dev server
make dev-down     # stop
```

`src/`, `lib/`, `main.c`, `Makefile`, `tools/`, `public/` are bind-mounted. The watcher polls mtimes every second (Docker Desktop on macOS doesn't forward inotify events). A small JS snippet injected before `</body>` polls `/__dev/ping` and reloads the page when the server comes back up.

---

## Docker Run (manual)

`make start` wraps this — only use it directly if you need custom flags:

```bash
docker run --rm -p 8080:8080 -p 8443:8443 \
  -v $(pwd)/data:/app/data \          # persist LMDB data
  --security-opt seccomp=unconfined \ # io_uring syscalls
  --cap-add SYS_NICE \                # IORING_SETUP_SQPOLL
  --ulimit nofile=65536:65536 \       # fd limit
  --ulimit memlock=-1 \               # fixed buffer memory lock
  cnext
```

---

## Project Structure

```
src/
  page.cxn              → GET /
  layout.cxn            → root layout, wraps every page at {{children}}
  path/
    page.cxn            → GET /path
    route.c             → GET + POST /path   (API)
    action.c            → POST /action/path/create
    action_delete.c     → POST /action/path/delete

lib/                    prebuilt framework (committed)
  libcnext.a            static library
  include/              public headers

.cnext/                 (gitignored — cxnc build output, like .next/ in Next.js)
  src/**/*_cxn.c        generated from src/**/*.cxn

tools/
  cxnc                  .cxn → .c compiler (Python)

public/
  style.css

main.c
Makefile
Dockerfile
```
