# ── Deps Stage — toolchain + libraries only (cached layer) ───────────
# Kept separate so lightweight targets (e.g. library-only builds) can
# start from here without triggering a full application compile.
FROM ubuntu:24.04 AS deps

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git ca-certificates python3 xxd openssl \
    liblmdb-dev libnghttp2-dev libcurl4-openssl-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Build liburing จาก source เพื่อให้ได้ recv_fixed / send_fixed
RUN git clone --depth 1 --branch liburing-2.6 \
      https://github.com/axboe/liburing.git /tmp/liburing \
 && cd /tmp/liburing \
 && ./configure --prefix=/usr/local \
 && make -j$(nproc) install \
 && ldconfig

# ── Build Stage ──────────────────────────────────────────────────────
FROM deps AS builder

WORKDIR /app
COPY . .
# If lib_dev/ is present (maintainer build), rebake lib/libcnext.a from source first;
# then the main Makefile (consumer) links the app against lib/libcnext.a.
RUN mkdir -p data \
 && if [ -f lib_dev/server.c ]; then \
      make -f Makefile.maintainer pack-native; \
    fi \
 && make clean && make

# ── Dev Stage — watch files, rebuild + restart on change ──────────────
# Used by `make dev` via compose.dev.yaml (bind-mounts src/, lib_dev/, etc.)
# Uses polling (not inotify) because Docker Desktop on macOS uses virtio-fs
# which doesn't forward inotify events from host file edits.
FROM builder AS dev
WORKDIR /app
EXPOSE 8080 8443
CMD ["sh", "tools/dev-watch.sh"]

# ── Runtime Stage ─────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    liblmdb0 libnghttp2-14 libcurl4 libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# copy liburing .so จาก builder
COPY --from=builder /usr/local/lib/liburing.so.2   /usr/local/lib/
COPY --from=builder /usr/local/lib/liburing.so.2.6 /usr/local/lib/
RUN ldconfig

WORKDIR /app

# binary
COPY --from=builder /app/server .

# assets ที่ server อ่านตอน runtime (lib/ และ src/ เป็น compile-time เท่านั้น)
COPY --from=builder /app/public/  ./public/

RUN mkdir -p data

EXPOSE 8080 8443

# วิธี run (full performance):
#   docker run --rm -p 8080:8080 \
#     -v $(pwd)/data:/app/data \
#     --security-opt seccomp=unconfined \
#     --cap-add SYS_NICE \
#     --ulimit nofile=65536:65536 \
#     --ulimit memlock=-1 \
#     cnext
#
# --security-opt seccomp=unconfined  → อนุญาต io_uring syscalls
# --cap-add SYS_NICE                 → อนุญาต IORING_SETUP_SQPOLL
# --ulimit nofile=65536:65536        → fd limit สำหรับ connections เยอะ
# --ulimit memlock=-1                → อนุญาต fixed buffers (lock memory)
# -v $(pwd)/data:/app/data           → persist LMDB files
#
CMD ["./server"]
