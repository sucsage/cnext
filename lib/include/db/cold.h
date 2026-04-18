#ifndef CNEXT_COLD_H
#define CNEXT_COLD_H

#include <stddef.h>

// mapsize_gb: ขนาด mmap สูงสุดของ database (ปรับได้ ค่า default = 10)
void  cold_init(const char *path, size_t mapsize_gb);
void  cold_destroy(void);

// Write เป็น async (ส่งเข้า queue → writer thread commit)
// คืน 0 = ส่งเข้า queue สำเร็จ, -1 = queue เต็ม (rare)
int   cold_put(const char *ns, const char *key, const char *val, size_t vlen);
int   cold_del(const char *ns, const char *key);

// Read เป็น sync (read-only txn, ถูกมาก)
// คืน malloc copy — caller ต้อง free()
// คืน NULL ถ้าไม่เจอ
char *cold_get(const char *ns, const char *key);

// Scan ทุก key-value ใน namespace (sync, read-only txn)
// cb ถูกเรียกทุก entry — key/val valid แค่ใน cb (อย่า keep pointer ไว้)
// คืน จำนวน entries ที่ scan, -1 ถ้า error
int   cold_scan(const char *ns,
                void (*cb)(const char *key, const char *val, void *ctx),
                void *ctx);

// Wait until all currently-enqueued writes are committed to LMDB
// Returns immediately if no pending writes (safe to call on every read)
void  cold_flush(void);

// Force flush ลง disk (ใช้ตอน shutdown หรือ checkpoint)
void  cold_sync(void);

#endif // CNEXT_COLD_H
