#ifndef CNEXT_HOT_H
#define CNEXT_HOT_H

#include <stddef.h>

void  hot_init(void);
void  hot_destroy(void);

// val ถูก copy เข้าไปเก็บ — caller ไม่ต้อง keep ไว้
int   hot_put(const char *ns, const char *key, const char *val, size_t vlen);

// คืน malloc copy — caller ต้อง free()
// คืน NULL ถ้าไม่เจอหรือ expired
char *hot_get(const char *ns, const char *key);

int   hot_del(const char *ns, const char *key);

// กำหนดอายุ (วินาที) — ใช้หลัง hot_put
int   hot_ttl(const char *ns, const char *key, int seconds);

#endif // CNEXT_HOT_H
