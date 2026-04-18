# cnext

Next.js-style SSR framework written in C — file-based routing, `.cxn` templates, server actions, compiled to a single binary.

```bash
docker build -t cnext .
docker run --rm -p 8080:8080 --security-opt seccomp=unconfined cnext
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

If `layout.cxn` is absent, pages render standalone via a weak default in `lib_dev/pages.c`.

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

```c
#include "fetch.h"

FetchResult *fr = cxn_fetch("https://api.example.com/data", "GET", NULL);
if (fr) {
    // fr->body, fr->len, fr->status
    fetch_free(fr);
}
```

`cxn_fetch` is blocking — call from a background thread to avoid blocking io_uring workers.

---

## Docker Run

```bash
docker run --rm -p 8080:8080 \
  -v $(pwd)/data:/app/data \          # persist LMDB data
  --security-opt seccomp=unconfined \ # io_uring syscalls
  --cap-add SYS_NICE \                # IORING_SETUP_SQPOLL
  --ulimit nofile=65536:65536 \       # fd limit
  --ulimit memlock=-1 \               # fixed buffer memory lock
  cnext
```

> **macOS:** io_uring is Linux-only. Always build and run via Docker.

---

## Build Modes

The framework ships the library as a prebuilt static archive so `lib_dev/*.c` stays private.

| Mode | When | How `make` resolves `libcnext.a` |
|------|------|----------------------------------|
| **source** | `lib_dev/` present on disk (dev machine) | Compile `lib_dev/*.c` → `libcnext.a` |
| **consumer** | `lib_dev/` absent (EC2, GitHub clone, CI) | Link `lib/libcnext.a` with `-Ilib/include` |

`lib_dev/` is gitignored — it is never pushed to GitHub. `lib/libcnext.a` + `lib/include/` is what the public repo ships.

### Dev workflow (push to GitHub)

```bash
# 1. edit lib_dev/*.c
# 2. refresh the published artifact
make lib-pack           # Linux
make pack-docker        # macOS (builds in Docker, copies back)
# 3. commit lib/ and push
git add lib src
git commit && git push
```

The GitHub Action verifies `lib/libcnext.a` exists before deploy — forgetting `pack` fails the build.

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

lib_dev/                (gitignored — private source)
  server.h / server.c   io_uring HTTP server + HTTP/1.1
  pages.h / pages.c     page engine
  h2.c                  HTTP/2 via nghttp2
  static.h / static.c   static file cache (RAM)
  fetch.h / fetch.c     libcurl wrapper
  db/
    db.h / db.c         two-tier API
    hot.h / hot.c       RAM cache (hash table)
    cold.h / cold.c     LMDB async writer

lib/                    (public artifact — committed)
  libcnext.a            prebuilt static library
  include/              public headers (mirror of lib_dev/*.h)

tools/
  cxnc                  .cxn → .c compiler (Python)

public/
  style.css

main.c
Makefile
Dockerfile
```
