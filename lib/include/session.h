#ifndef CNEXT_SESSION_H
#define CNEXT_SESSION_H

#include "server.h"

// =====================================================================
// session — cookie-based server-side state (backed by db_hot)
// ---------------------------------------------------------------------
// Usage:
//   const char *sid = session_id(req);
//   char *cart = session_get(req, "cart");          // malloc'd; free()
//   session_set(req, "cart", "[{...}]", 3600);      // ttl seconds; 0 = default
//   session_del(req, "cart");
// ---------------------------------------------------------------------
// Cookie: "cnext_sid=<32-hex>; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400"
// Storage namespace in db_hot: "sess:<sid>"
// =====================================================================

#define SESSION_COOKIE    "cnext_sid"
#define SESSION_TTL_COOKIE 86400     // 1 day

// Returns a stable 32-hex session id for this request. If the client
// had no cookie, a new id is generated and a Set-Cookie header is
// injected into the current response.
const char *session_id(HttpRequest *req);

// Key/value store keyed by SID. session_get returns a malloc'd copy
// (caller frees), or NULL if absent/expired. ttl in seconds; 0 → 1 day.
char *session_get(HttpRequest *req, const char *key);
int   session_set(HttpRequest *req, const char *key, const char *val, int ttl);
int   session_del(HttpRequest *req, const char *key);

#endif // CNEXT_SESSION_H
