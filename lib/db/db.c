#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"
#include "hot.h"
#include "cold.h"

// =====================================================================
// Lifecycle
// =====================================================================
int db_init(const char *db_path, size_t mapsize_gb)
{
    hot_init();
    cold_init(db_path, mapsize_gb);
    return 0;
}

void db_close(void)
{
    db_sync();
    cold_destroy();
    hot_destroy();
}

// =====================================================================
// List cache helpers
// =====================================================================
#define LIST_CACHE_NS "__cache__"
#define LIST_KEY_MAX 192

static void list_cache_key(char *out, const char *ns)
{
    snprintf(out, LIST_KEY_MAX, "list__%s", ns);
}

static void list_cache_invalidate(const char *ns)
{
    char ck[LIST_KEY_MAX];
    list_cache_key(ck, ns);
    hot_del(LIST_CACHE_NS, ck);
}

// =====================================================================
// Put
//
// DB_HOT  → hot เท่านั้น (ไม่ persistent)
// DB_COLD → cold เท่านั้น (async write to disk)
// DB_BOTH → hot ทันที + async cold (เร็ว + durable)
//
// DB_COLD / DB_BOTH → invalidate list cache ของ namespace นั้นด้วย
// =====================================================================
int db_put(DbTier tier, const char *ns, const char *key, const char *val)
{
    size_t vlen = strlen(val);
    int rc = 0;

    if (tier == DB_HOT || tier == DB_BOTH)
    {
        if (hot_put(ns, key, val, vlen) != 0)
            rc = -1;
    }
    if (tier == DB_COLD || tier == DB_BOTH)
    {
        
        if (cold_put(ns, key, val, vlen) != 0)
            rc = -1;
        list_cache_invalidate(ns);
    }
    return rc;
}

// =====================================================================
// Get — two-tier lookup
//
// 1. ดู hot tier ก่อน → nanoseconds
// 2. miss → ดู cold tier → microseconds
// 3. cold hit → warm hot cache สำหรับ request ต่อไป
//
// คืน malloc copy — caller ต้อง free()
// =====================================================================
char *db_get(const char *ns, const char *key)
{
    char *val = hot_get(ns, key);
    if (val)
        return val;

    val = cold_get(ns, key);
    if (!val)
        return NULL;

    hot_put(ns, key, val, strlen(val));
    return val;
}

// =====================================================================
// Del — ลบจากทั้งสอง tier + invalidate list cache
// =====================================================================
int db_del(const char *ns, const char *key)
{
    hot_del(ns, key);
    cold_del(ns, key);
    list_cache_invalidate(ns);
    return 0;
}

// =====================================================================
// TTL — ใช้ได้กับ hot tier เท่านั้น
// =====================================================================
int db_ttl(const char *ns, const char *key, int seconds)
{
    return hot_ttl(ns, key, seconds);
}

// =====================================================================
// Scan
// =====================================================================
int db_scan(const char *ns,
            void (*cb)(const char *key, const char *val, void *ctx),
            void *ctx)
{
    return cold_scan(ns, cb, ctx);
}

// =====================================================================
// List — JSON array "[v1,v2,...]" พร้อม hot cache
// =====================================================================
typedef struct
{
    char buf[65536];
    size_t len;
    int first;
} ListBuf;

static void list_collect(const char *key, const char *val, void *arg)
{
    (void)key;
    ListBuf *b = arg;
    size_t vlen = strlen(val);
    if (!b->first && b->len + 1 < sizeof(b->buf))
        b->buf[b->len++] = ',';
    if (b->len + vlen < sizeof(b->buf))
    {
        memcpy(b->buf + b->len, val, vlen);
        b->len += vlen;
        b->first = 0;
    }
}

char *db_list(const char *ns)
{
    char ck[LIST_KEY_MAX];
    list_cache_key(ck, ns);

    // hot cache hit
    char *cached = hot_get(LIST_CACHE_NS, ck);
    if (cached)
        return cached;

    // miss → scan cold
    ListBuf b;
    b.buf[0] = '[';
    b.len = 1;
    b.first = 1;
    cold_scan(ns, list_collect, &b);
    b.buf[b.len++] = ']';
    b.buf[b.len] = '\0';

    // warm cache
    hot_put(LIST_CACHE_NS, ck, b.buf, b.len);

    return strdup(b.buf);
}

// =====================================================================
// Sync
// =====================================================================
void db_sync(void)
{
    cold_sync();
}
