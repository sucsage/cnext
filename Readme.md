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
| `@meta key "value"` | Per-page metadata (`title`, `description`, `og_image`, `canonical`) |
| `{{children}}` | (in `layout.cxn` only) slot where the page body renders |
| `<%! ... %>` | C code at file scope (close with `%>` on its own line) |
| `<% code; %>` | C code inside `render()` |
| `{{expr}}` | `page_writef(ctx, "%s", expr)` |
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

Four APIs — pick by context:

| API | Blocks? | Cached | Use from |
|---|---|---|---|
| `cxn_fetch` | yes | no | action handlers (they block anyway — user waits for redirect) |
| `cxn_fetch_async` | no | no | fire-and-forget (analytics, webhooks) |
| `cxn_fetch_cached` | no | yes (raw body) | `page.cxn` render path |
| `cxn_fetch_cached_typed` | no | yes (parsed struct) | `page.cxn` — zero-parse hit path |

All async/cached calls share an internal **thread pool** (4 workers by default) + **in-RAM cache** with **stale-while-revalidate**. Thread-local `CURL` handle reuses TCP/TLS keep-alive across calls.

### Blocking — `cxn_fetch`

```c
#include "fetch.h"

FetchResult *fr = cxn_fetch("https://api.example.com/data", "GET", NULL);
if (fr) {
    // fr->body, fr->len, fr->status
    fetch_free(fr);
}
```

Supports `"GET"`, `"POST"`, `"PUT"`, `"PATCH"`, `"DELETE"` (or any custom method). `body` is `NULL` for GET/DELETE. **Never call from a page render** — it will block the io_uring worker.

### Fire-and-forget — `cxn_fetch_async`

```c
cxn_fetch_async("https://analytics.example.com/event", "POST",
                "{\"event\":\"page_view\"}", NULL, NULL);
```

Returns instantly. Optional callback runs on a worker thread when done — do not touch the originating request's `PageCtx` or socket from inside.

### Cached raw body — `cxn_fetch_cached`

```c
<% char *data = cxn_fetch_cached("https://api.example.com/todos", 60); %>
<div>{{ data ? data : "loading..." }}</div>
<% free(data); %>
```

- **Fresh hit** (< `ttl`) → returns `strdup(body)` immediately
- **Stale hit** (`ttl` ≤ age < `2×ttl`) → returns stale copy + spawns background refresh
- **Miss** → returns `NULL` + spawns background fetch; next request gets data

Caller always `free()`s the returned string.

### Cached typed — `cxn_fetch_cached_typed`

Parse JSON into a struct once, cache the binary struct, render path is pure `memcpy`.

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
   int ok = cxn_fetch_cached_typed(
       "https://jsonplaceholder.typicode.com/todos/1", 60,
       &t, sizeof(t), TODO_FIELDS, TODO_NFIELDS);
%>

<div>
  <% if (ok) { %>
    <div>#{{t.id}} — {{t.title}}</div>
    <div>{{ t.completed ? "done" : "pending" }}</div>
  <% } else { %>
    <div>loading...</div>
  <% } %>
</div>
```

Field types: `F_STR` (char array in-struct), `F_INT`, `F_I64`, `F_F64`, `F_BOOL`. Nested JSON objects/arrays are skipped — declare only the top-level keys you need.

### Standalone JSON parse — `cxn_json_parse`

If you already have a body string:

```c
Todo t = {0};
cxn_json_parse(body, len, &t, TODO_FIELDS, TODO_NFIELDS);
```

One-pass tokenizer, zero heap allocation.

### Performance notes

- Cached hit path: hash lookup + `memcpy` → sub-µs (no JSON re-parse)
- Thread pool shares a 1024-slot hash cache (FNV-1a, open-addressing)
- In-flight dedup: 1000 concurrent requests for the same URL fire **one** backend call
- TLS/TCP keep-alive is reused per worker thread via `curl_easy_reset` on a pinned `CURL *`
- Stale-while-revalidate: users never block on a refresh — they always get fresh or stale-served data

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
