#ifndef CNEXT_DB_H
#define CNEXT_DB_H

#include <stddef.h>

// =====================================================================
// DbTier — บอกว่าจะเก็บ data ที่ไหน
// =====================================================================
typedef enum {
    DB_HOT,   // RAM เท่านั้น  — session, rate-limit, cache (ไม่ durable)
    DB_COLD,  // Disk เท่านั้น — user data, content (durable)
    DB_BOTH,  // RAM + Disk    — hot user data (เร็ว + durable)
} DbTier;

// =====================================================================
// Lifecycle
// db_path: directory สำหรับ LMDB files (ต้องมีอยู่แล้ว)
// mapsize_gb: ขนาด mmap สูงสุด (default แนะนำ 10)
// =====================================================================
int  db_init(const char *db_path, size_t mapsize_gb);
void db_close(void);

// =====================================================================
// Core API
//
// ns  : namespace เช่น "users", "sessions", "cache"
// key : string key
// val : string value (จะถูก copy — caller ไม่ต้อง keep ไว้)
//
// db_get คืน malloc copy → caller ต้อง free()
//         ดู hot tier ก่อน ถ้า miss → ดู cold tier แล้ว warm cache
//         คืน NULL ถ้าไม่เจอ
// =====================================================================
int   db_put(DbTier tier, const char *ns, const char *key, const char *val);
char *db_get(const char *ns, const char *key);
int   db_del(const char *ns, const char *key);

// กำหนดอายุ (วินาที) สำหรับ hot tier — ใช้หลัง db_put(DB_HOT/DB_BOTH, ...)
int   db_ttl(const char *ns, const char *key, int seconds);

// Force flush cold store ลง disk
void  db_sync(void);

#endif // CNEXT_DB_H
