#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "hot.h"

// =====================================================================
// Config
// =====================================================================
#define HOT_CAP      (1 << 13)          // 8192 slots (ลดจาก 65536 ลด BSS ~20MB)
#define HOT_MASK     (HOT_CAP - 1)
#define LOAD_HIGH    (HOT_CAP * 85 / 100)
#define KEY_MAX      320                 // "ns\x01key" composite

// =====================================================================
// Slot
// =====================================================================
typedef enum { SLOT_EMPTY = 0, SLOT_USED, SLOT_DEAD } SlotState;

typedef struct {
    char        key[KEY_MAX];
    char       *val;
    size_t      vlen;
    time_t      exp;        // 0 = ไม่มี TTL
    uint64_t    lru;
    SlotState   state;
} Slot;

// =====================================================================
// State
// =====================================================================
static Slot              table[HOT_CAP];
static size_t            used;
static uint64_t          tick;
static pthread_rwlock_t  rwlock    = PTHREAD_RWLOCK_INITIALIZER;
static pthread_once_t    evict_once = PTHREAD_ONCE_INIT;

// =====================================================================
// FNV-1a hash
// =====================================================================
static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h & HOT_MASK;
}

static void make_key(char *out, const char *ns, const char *key) {
    size_t nlen = strlen(ns);
    size_t klen = strlen(key);
    if (nlen + 1 + klen >= KEY_MAX) nlen = KEY_MAX - klen - 2;
    memcpy(out, ns, nlen);
    out[nlen] = '\x01';
    memcpy(out + nlen + 1, key, klen + 1);  // +1 รวม '\0'
}

// =====================================================================
// Eviction — ต้องถือ write lock ก่อนเรียก
// Pass 1: ลบ expired ทั้งหมด
// Pass 2: ถ้ายังเต็ม ลบ LRU 10%
// =====================================================================
static void evict(void) {
    time_t now = time(NULL);

    for (int i = 0; i < HOT_CAP; i++) {
        if (table[i].state == SLOT_USED &&
            table[i].exp && now > table[i].exp) {
            free(table[i].val);
            table[i].val   = NULL;
            table[i].state = SLOT_DEAD;
            used--;
        }
    }
    if (used < LOAD_HIGH) return;

    // LRU: หา threshold = bottom 20% ของ tick range
    uint64_t min_lru = UINT64_MAX, max_lru = 0;
    for (int i = 0; i < HOT_CAP; i++) {
        if (table[i].state != SLOT_USED) continue;
        if (table[i].lru < min_lru) min_lru = table[i].lru;
        if (table[i].lru > max_lru) max_lru = table[i].lru;
    }
    uint64_t threshold = min_lru + (max_lru - min_lru) / 5;

    for (int i = 0; i < HOT_CAP && used >= LOAD_HIGH; i++) {
        if (table[i].state == SLOT_USED && table[i].lru <= threshold) {
            free(table[i].val);
            table[i].val   = NULL;
            table[i].state = SLOT_DEAD;
            used--;
        }
    }
}

// =====================================================================
// Background eviction thread — ทำงานทุก 5 วินาที
// hot_put ไม่ต้อง evict เองภายใต้ write lock อีกต่อไป
// (ยกเว้น emergency: table เต็ม > 95%)
// =====================================================================
#define EVICT_INTERVAL_SEC 5
#define LOAD_EMERGENCY     (HOT_CAP * 95 / 100)

static void *evict_worker(void *arg) {
    (void)arg;
    for (;;) {
        sleep(EVICT_INTERVAL_SEC);
        pthread_rwlock_wrlock(&rwlock);
        evict();
        pthread_rwlock_unlock(&rwlock);
    }
    return NULL;
}

static void start_evict_thread(void) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, evict_worker, NULL);
    pthread_attr_destroy(&attr);
}

// =====================================================================
// Init / Destroy
// =====================================================================
void hot_init(void) {
    memset(table, 0, sizeof(table));
    used = 0;
    tick = 0;
    pthread_once(&evict_once, start_evict_thread);
    printf("[cnext-db] hot store ready  cap=%d slots\n", HOT_CAP);
}

void hot_destroy(void) {
    pthread_rwlock_wrlock(&rwlock);
    for (int i = 0; i < HOT_CAP; i++) {
        if (table[i].state == SLOT_USED) { free(table[i].val); }
    }
    memset(table, 0, sizeof(table));
    used = 0;
    pthread_rwlock_unlock(&rwlock);
}

// =====================================================================
// Put
// =====================================================================
int hot_put(const char *ns, const char *key, const char *val, size_t vlen) {
    char fk[KEY_MAX];
    make_key(fk, ns, key);

    char *copy = malloc(vlen + 1);
    if (!copy) return -1;
    memcpy(copy, val, vlen);
    copy[vlen] = '\0';

    pthread_rwlock_wrlock(&rwlock);

    // background thread จัดการ evict ทุก 5s
    // inline evict เฉพาะ emergency (>95%) เพื่อป้องกัน table เต็ม
    if (used >= LOAD_EMERGENCY) evict();

    uint32_t h = fnv1a(fk);
    int first_dead = -1;

    for (int i = 0; i < HOT_CAP; i++) {
        int idx = (h + i) & HOT_MASK;
        Slot *s = &table[idx];

        if (s->state == SLOT_EMPTY) {
            // ใช้ dead slot ที่เจอก่อนถ้ามี
            if (first_dead >= 0) { s = &table[first_dead]; }
            s->val   = copy;
            s->vlen  = vlen;
            s->exp   = 0;
            s->lru   = ++tick;
            s->state = SLOT_USED;
            snprintf(s->key, KEY_MAX, "%s", fk);
            used++;
            pthread_rwlock_unlock(&rwlock);
            return 0;
        }
        if (s->state == SLOT_DEAD && first_dead < 0) {
            first_dead = idx;
            continue;
        }
        if (s->state == SLOT_USED && strcmp(s->key, fk) == 0) {
            // อัปเดต key เดิม
            free(s->val);
            s->val  = copy;
            s->vlen = vlen;
            s->exp  = 0;
            s->lru  = ++tick;
            pthread_rwlock_unlock(&rwlock);
            return 0;
        }
    }

    // ถ้า probe ครบแล้วยังไม่เจอ empty ให้ใช้ dead slot
    if (first_dead >= 0) {
        Slot *s = &table[first_dead];
        s->val   = copy;
        s->vlen  = vlen;
        s->exp   = 0;
        s->lru   = ++tick;
        s->state = SLOT_USED;
        snprintf(s->key, KEY_MAX, "%s", fk);
        used++;
        pthread_rwlock_unlock(&rwlock);
        return 0;
    }

    pthread_rwlock_unlock(&rwlock);
    free(copy);
    return -1; // table full (ไม่ควรเกิด)
}

// =====================================================================
// Get — คืน malloc copy, caller ต้อง free()
// =====================================================================
char *hot_get(const char *ns, const char *key) {
    char fk[KEY_MAX];
    make_key(fk, ns, key);
    pthread_rwlock_rdlock(&rwlock);

    uint32_t h = fnv1a(fk);
    for (int i = 0; i < HOT_CAP; i++) {
        int   idx = (h + i) & HOT_MASK;
        Slot *s   = &table[idx];

        if (s->state == SLOT_EMPTY) break;
        if (s->state == SLOT_DEAD)  continue;
        if (strcmp(s->key, fk) != 0) continue;

        // พบ key
        if (s->exp && time(NULL) > s->exp) {
            // Expired — ต้องการ write lock เพื่อลบ
            pthread_rwlock_unlock(&rwlock);
            pthread_rwlock_wrlock(&rwlock);
            // re-check หลัง upgrade
            if (s->state == SLOT_USED && strcmp(s->key, fk) == 0 &&
                s->exp && time(NULL) > s->exp) {
                free(s->val);
                s->val   = NULL;
                s->state = SLOT_DEAD;
                used--;
            }
            pthread_rwlock_unlock(&rwlock);
            return NULL;
        }

        char *result = malloc(s->vlen + 1);
        if (result) {
            memcpy(result, s->val, s->vlen);
            result[s->vlen] = '\0';
        }
        // อัปเดต LRU แบบ atomic ไม่ต้อง upgrade lock
        __atomic_fetch_add(&tick, 1, __ATOMIC_RELAXED);
        s->lru = tick;
        pthread_rwlock_unlock(&rwlock);
        return result;
    }

    pthread_rwlock_unlock(&rwlock);
    return NULL;
}

// =====================================================================
// Del
// =====================================================================
int hot_del(const char *ns, const char *key) {
    char fk[KEY_MAX];
    make_key(fk, ns, key);

    pthread_rwlock_wrlock(&rwlock);

    uint32_t h = fnv1a(fk);
    for (int i = 0; i < HOT_CAP; i++) {
        int   idx = (h + i) & HOT_MASK;
        Slot *s   = &table[idx];

        if (s->state == SLOT_EMPTY) break;
        if (s->state == SLOT_DEAD)  continue;
        if (strcmp(s->key, fk) == 0) {
            free(s->val);
            s->val   = NULL;
            s->state = SLOT_DEAD;
            used--;
            pthread_rwlock_unlock(&rwlock);
            return 0;
        }
    }

    pthread_rwlock_unlock(&rwlock);
    return -1;
}

// =====================================================================
// TTL
// =====================================================================
int hot_ttl(const char *ns, const char *key, int seconds) {
    char fk[KEY_MAX];
    make_key(fk, ns, key);

    pthread_rwlock_wrlock(&rwlock);

    uint32_t h = fnv1a(fk);
    for (int i = 0; i < HOT_CAP; i++) {
        int   idx = (h + i) & HOT_MASK;
        Slot *s   = &table[idx];

        if (s->state == SLOT_EMPTY) break;
        if (s->state == SLOT_DEAD)  continue;
        if (strcmp(s->key, fk) == 0) {
            s->exp = time(NULL) + seconds;
            pthread_rwlock_unlock(&rwlock);
            return 0;
        }
    }

    pthread_rwlock_unlock(&rwlock);
    return -1;
}
