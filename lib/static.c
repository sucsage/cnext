#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "static.h"
#include "server.h"

#define PUBLIC_DIR  "public"
#define MAX_FILES   64
#define MAX_FILE_SIZE (1024 * 1024)  // 1MB

// =====================================================================
// In-memory file cache — โหลดทุกไฟล์ใน public/ ตอน startup
// GET /style.css → serve จาก RAM ไม่แตะ disk เลย
// =====================================================================
typedef struct {
    char        url[512];    // เช่น "/style.css"
    char       *data;        // file content
    size_t      size;
    const char *mime;
} CachedFile;

static CachedFile cache[MAX_FILES];
static int        cache_count = 0;

static const char *mime_of(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".woff2")== 0) return "font/woff2";
    return "application/octet-stream";
}

static void cache_file(const char *filepath, const char *url) {
    if (cache_count >= MAX_FILES) return;

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > MAX_FILE_SIZE) {
        close(fd); return;
    }

    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return; }

    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (n < 0) { free(buf); return; }
    buf[n] = '\0';

    CachedFile *cf = &cache[cache_count++];
    memcpy(cf->url, url, strlen(url) + 1);
    cf->data = buf;
    cf->size = (size_t)n;
    cf->mime = mime_of(url);

    printf("[static] cached %-30s %zu bytes\n", url, cf->size);
}

// =====================================================================
// Static File Handler — O(n) lookup แต่ไฟล์น้อย (< 64) → ไม่เป็นไร
// =====================================================================
static void static_serve(const char *url_path, int socket_fd) {
    if (strstr(url_path, "..") || strstr(url_path, "//")) {
        send_response(socket_fd, 403, "Forbidden", "text/plain", "Forbidden\n");
        return;
    }

    for (int i = 0; i < cache_count; i++) {  // O(n) — ไฟล์น้อย (<64) รับได้
        if (strcmp(cache[i].url, url_path) == 0) {
            send_response(socket_fd, 200, "OK", cache[i].mime, cache[i].data);
            return;
        }
    }

    send_response(socket_fd, 404, "Not Found", "text/plain", "File not found\n");
}

// =====================================================================
// static_init — scan public/ และ cache ทุกไฟล์เข้า RAM
// =====================================================================
void static_init(void) {
    DIR *dir = opendir(PUBLIC_DIR);
    if (!dir) { fprintf(stderr, "[static] cannot open '%s/'\n", PUBLIC_DIR); return; }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char filepath[512], url[512];  // 512 — รองรับ d_name ยาวสุด 255 + "/" + null
        snprintf(filepath, sizeof(filepath), "%s/%s", PUBLIC_DIR, ent->d_name);
        snprintf(url,      sizeof(url),      "/%s",   ent->d_name);
        cache_file(filepath, url);
    }
    closedir(dir);

    set_static_fallback(static_serve);
    printf("[static] %d file(s) cached from %s/\n", cache_count, PUBLIC_DIR);
}
