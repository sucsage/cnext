#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <lmdb.h>
#include "cold.h"

// =====================================================================
// Config
// =====================================================================
#define WQ_SIZE      4096          // write queue — power of 2
#define WQ_MASK      (WQ_SIZE - 1)
#define MAX_DBS      32            // namespace สูงสุด (= named MDB_dbi)
#define MAX_NS       64
#define MAX_KEY      256
#define BATCH_MAX    256           // commit ทุก N jobs หรือเมื่อ queue ว่าง

// =====================================================================
// Write Queue
// =====================================================================
typedef enum { WQ_PUT = 0, WQ_DEL } WqOp;

typedef struct {
    WqOp   op;
    char   ns[MAX_NS];
    char   key[MAX_KEY];
    char  *val;          // heap — writer thread จะ free()
    size_t vlen;
} WriteJob;

static WriteJob        wq[WQ_SIZE];
static int             wq_head    = 0;  // producer เขียนที่นี่
static int             wq_tail    = 0;  // consumer อ่านที่นี่
static int             wq_flushed = 0;  // jobs ที่ commit สำเร็จแล้ว (monotonic)
static pthread_mutex_t wq_mu    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wq_data  = PTHREAD_COND_INITIALIZER;  // มี data ใหม่
static pthread_cond_t  wq_space = PTHREAD_COND_INITIALIZER;  // มี space ว่าง
static pthread_cond_t  wq_done  = PTHREAD_COND_INITIALIZER;  // commit เสร็จ

// =====================================================================
// LMDB State
// =====================================================================
typedef struct { char ns[MAX_NS]; MDB_dbi dbi; } DbiEntry;

static MDB_env    *env;
static DbiEntry    dbi_cache[MAX_DBS];
static int         dbi_count = 0;
static pthread_t   writer_tid;
static volatile int running = 1;

// =====================================================================
// Namespace → MDB_dbi mapping (เรียกใน write txn เท่านั้น)
// =====================================================================
static MDB_dbi open_dbi(MDB_txn *txn, const char *ns) {
    for (int i = 0; i < dbi_count; i++) {
        if (strcmp(dbi_cache[i].ns, ns) == 0)
            return dbi_cache[i].dbi;
    }
    MDB_dbi dbi;
    if (mdb_dbi_open(txn, ns, MDB_CREATE, &dbi) != MDB_SUCCESS) return 0;
    if (dbi_count < MAX_DBS) {
        strncpy(dbi_cache[dbi_count].ns, ns, MAX_NS - 1);
        dbi_cache[dbi_count].dbi = dbi;
        dbi_count++;
    }
    return dbi;
}

// =====================================================================
// Writer Thread — single writer, batch commit
// =====================================================================
static void *writer_thread(void *arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&wq_mu);

        // รอจนมี job หรือ shutdown
        while (wq_head == wq_tail && running)
            pthread_cond_wait(&wq_data, &wq_mu);

        if (!running && wq_head == wq_tail) {
            pthread_mutex_unlock(&wq_mu);
            break;
        }

        // เริ่ม write transaction
        MDB_txn *txn;
        if (mdb_txn_begin(env, NULL, 0, &txn) != MDB_SUCCESS) {
            pthread_mutex_unlock(&wq_mu);
            continue;
        }

        int batch = 0;
        while (wq_tail != wq_head && batch < BATCH_MAX) {
            WriteJob *job = &wq[wq_tail & WQ_MASK];

            MDB_dbi  dbi = open_dbi(txn, job->ns);
            MDB_val  k   = { strlen(job->key), job->key };

            if (job->op == WQ_PUT && job->val) {
                MDB_val v = { job->vlen, job->val };
                mdb_put(txn, dbi, &k, &v, 0);
            } else {
                mdb_del(txn, dbi, &k, NULL);
            }

            free(job->val);
            job->val = NULL;
            wq_tail++;
            batch++;
        }

        int committed_tail = wq_tail;        // บันทึกค่าก่อน unlock
        pthread_cond_broadcast(&wq_space);   // บอก producer ว่ามี space แล้ว
        pthread_mutex_unlock(&wq_mu);

        mdb_txn_commit(txn);                 // commit นอก lock → ไม่บล็อก producer

        // แจ้ง cold_flush() ว่า jobs ถึง committed_tail ถูก commit แล้ว
        pthread_mutex_lock(&wq_mu);
        wq_flushed = committed_tail;
        pthread_cond_broadcast(&wq_done);
        pthread_mutex_unlock(&wq_mu);
    }

    // Flush ที่เหลือก่อน exit
    if (wq_tail != wq_head) {
        MDB_txn *txn;
        if (mdb_txn_begin(env, NULL, 0, &txn) == MDB_SUCCESS) {
            while (wq_tail != wq_head) {
                WriteJob *job = &wq[wq_tail & WQ_MASK];
                MDB_dbi  dbi  = open_dbi(txn, job->ns);
                MDB_val  k    = { strlen(job->key), job->key };
                if (job->op == WQ_PUT && job->val) {
                    MDB_val v = { job->vlen, job->val };
                    mdb_put(txn, dbi, &k, &v, 0);
                } else {
                    mdb_del(txn, dbi, &k, NULL);
                }
                free(job->val);
                wq_tail++;
            }
            mdb_txn_commit(txn);
        }
    }
    return NULL;
}

// =====================================================================
// Init / Destroy
// =====================================================================
void cold_init(const char *path, size_t mapsize_gb) {
    mdb_env_create(&env);
    mdb_env_set_mapsize(env, mapsize_gb * 1024UL * 1024UL * 1024UL);
    mdb_env_set_maxdbs(env, MAX_DBS);

    // MDB_NOTLS   — txn ไม่ผูกกับ thread (สำคัญมากสำหรับ multi-worker)
    // MDB_MAPASYNC — async mmap flush (เร็วกว่า fully-sync แต่ปลอดภัยกว่า NOSYNC)
    // หมายเหตุ: ต้องใช้ named Docker volume (ไม่ใช่ bind mount จาก macOS)
    //           bind mount บน virtiofs ทำให้ file locking ของ LMDB ทำงานผิดปกติ
    int rc = mdb_env_open(env, path, MDB_NOTLS | MDB_MAPASYNC, 0664);
    if (rc != MDB_SUCCESS) {
        fprintf(stderr, "[cnext-db] cold_init failed: %s\n", mdb_strerror(rc));
        exit(EXIT_FAILURE);
    }

    // สร้าง writer thread
    pthread_create(&writer_tid, NULL, writer_thread, NULL);
    printf("[cnext-db] cold store ready  path=%s  mapsize=%zuGB\n",
           path, mapsize_gb);
}

void cold_destroy(void) {
    // บอก writer ให้ flush แล้วหยุด
    pthread_mutex_lock(&wq_mu);
    running = 0;
    pthread_cond_signal(&wq_data);
    pthread_mutex_unlock(&wq_mu);

    pthread_join(writer_tid, NULL);
    mdb_env_sync(env, 1);
    mdb_env_close(env);
}

// =====================================================================
// Put (async — non-blocking สำหรับ io_uring workers)
// =====================================================================
int cold_put(const char *ns, const char *key, const char *val, size_t vlen) {
    char *copy = malloc(vlen + 1);
    if (!copy) return -1;
    memcpy(copy, val, vlen);
    copy[vlen] = '\0';

    pthread_mutex_lock(&wq_mu);

    // รอถ้า queue เต็ม (rare case)
    while ((wq_head - wq_tail) >= WQ_SIZE)
        pthread_cond_wait(&wq_space, &wq_mu);

    WriteJob *job = &wq[wq_head & WQ_MASK];
    job->op  = WQ_PUT;
    strncpy(job->ns,  ns,  MAX_NS  - 1); job->ns[MAX_NS   - 1] = '\0';
    strncpy(job->key, key, MAX_KEY - 1); job->key[MAX_KEY - 1] = '\0';
    job->val  = copy;
    job->vlen = vlen;
    wq_head++;

    pthread_cond_signal(&wq_data);
    pthread_mutex_unlock(&wq_mu);
    return 0;
}

// =====================================================================
// Del (async)
// =====================================================================
int cold_del(const char *ns, const char *key) {
    pthread_mutex_lock(&wq_mu);

    while ((wq_head - wq_tail) >= WQ_SIZE)
        pthread_cond_wait(&wq_space, &wq_mu);

    WriteJob *job = &wq[wq_head & WQ_MASK];
    job->op  = WQ_DEL;
    strncpy(job->ns,  ns,  MAX_NS  - 1); job->ns[MAX_NS   - 1] = '\0';
    strncpy(job->key, key, MAX_KEY - 1); job->key[MAX_KEY - 1] = '\0';
    job->val  = NULL;
    job->vlen = 0;
    wq_head++;

    pthread_cond_signal(&wq_data);
    pthread_mutex_unlock(&wq_mu);
    return 0;
}

// =====================================================================
// Get (sync read-only txn — ถูกมาก ไม่บล็อก writer)
// =====================================================================
char *cold_get(const char *ns, const char *key) {
    MDB_txn *txn;
    if (mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS)
        return NULL;

    MDB_dbi dbi;
    if (mdb_dbi_open(txn, ns, 0, &dbi) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return NULL;
    }

    MDB_val k = { strlen(key), (void *)key };
    MDB_val v;
    char *result = NULL;

    if (mdb_get(txn, dbi, &k, &v) == MDB_SUCCESS) {
        result = malloc(v.mv_size + 1);
        if (result) {
            memcpy(result, v.mv_data, v.mv_size);
            result[v.mv_size] = '\0';
        }
    }

    mdb_txn_abort(txn);  // read-only → abort = free txn slot
    return result;
}

// =====================================================================
// Scan (sync read-only cursor)
// =====================================================================
int cold_scan(const char *ns,
              void (*cb)(const char *key, const char *val, void *ctx),
              void *ctx) {
    MDB_txn *txn;
    if (mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) != MDB_SUCCESS)
        return -1;

    MDB_dbi dbi;
    if (mdb_dbi_open(txn, ns, 0, &dbi) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return 0;   // namespace ว่าง = ไม่ใช่ error
    }

    MDB_cursor *cur;
    if (mdb_cursor_open(txn, dbi, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return -1;
    }

    MDB_val k, v;
    int count = 0;
    char key_buf[MAX_KEY];

    while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == MDB_SUCCESS) {
        size_t klen = k.mv_size < MAX_KEY - 1 ? k.mv_size : MAX_KEY - 1;
        memcpy(key_buf, k.mv_data, klen);
        key_buf[klen] = '\0';

        // val ชี้ตรงไปที่ mmap — valid ตลอด txn, ไม่ต้อง malloc
        char *val = (char *)v.mv_data;
        char  saved = val[v.mv_size];   // เก็บ byte หลัง val ไว้
        // LMDB mmap ไม่รับประกัน null-terminator → ต้อง copy
        char *val_copy = malloc(v.mv_size + 1);
        if (val_copy) {
            memcpy(val_copy, val, v.mv_size);
            val_copy[v.mv_size] = '\0';
            cb(key_buf, val_copy, ctx);
            free(val_copy);
            count++;
        }
        (void)saved;
    }

    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return count;
}

// =====================================================================
// Flush — รอให้ทุก job ที่ enqueue ไว้แล้ว commit สำเร็จก่อน return
// ถ้าไม่มี pending jobs → return ทันที (wq_flushed == wq_head)
// =====================================================================
void cold_flush(void) {
    pthread_mutex_lock(&wq_mu);
    int target = wq_head;
    while (wq_flushed < target)
        pthread_cond_wait(&wq_done, &wq_mu);
    pthread_mutex_unlock(&wq_mu);
}

// =====================================================================
// Sync (force flush to disk)
// =====================================================================
void cold_sync(void) {
    mdb_env_sync(env, 1);
}
