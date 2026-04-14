#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"
#include "hot.h"
#include "cold.h"

// =====================================================================
// Lifecycle
// =====================================================================
int db_init(const char *db_path, size_t mapsize_gb) {
    hot_init();
    cold_init(db_path, mapsize_gb);
    return 0;
}

void db_close(void) {
    db_sync();
    cold_destroy();
    hot_destroy();
}

// =====================================================================
// Put
//
// DB_HOT  → hot เท่านั้น (ไม่ persistent)
// DB_COLD → cold เท่านั้น (async write to disk)
// DB_BOTH → hot ทันที + async cold (เร็ว + durable)
// =====================================================================
int db_put(DbTier tier, const char *ns, const char *key, const char *val) {
    size_t vlen = strlen(val);
    int    rc   = 0;

    if (tier == DB_HOT || tier == DB_BOTH) {
        if (hot_put(ns, key, val, vlen) != 0) rc = -1;
    }
    if (tier == DB_COLD || tier == DB_BOTH) {
        if (cold_put(ns, key, val, vlen) != 0) rc = -1;
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
char *db_get(const char *ns, const char *key) {
    char *val = hot_get(ns, key);
    if (val) return val;

    val = cold_get(ns, key);
    if (!val) return NULL;

    hot_put(ns, key, val, strlen(val));
    return val;
}

// =====================================================================
// Del — ลบจากทั้งสอง tier
// =====================================================================
int db_del(const char *ns, const char *key) {
    hot_del(ns, key);    // ไม่สนใจ error (อาจไม่มีใน hot)
    cold_del(ns, key);   // async
    return 0;
}

// =====================================================================
// TTL — ใช้ได้กับ hot tier เท่านั้น
// =====================================================================
int db_ttl(const char *ns, const char *key, int seconds) {
    return hot_ttl(ns, key, seconds);
}

// =====================================================================
// Sync
// =====================================================================
void db_sync(void) {
    cold_sync();
}
