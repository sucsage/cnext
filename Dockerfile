# ── Build Stage ──────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git ca-certificates python3 \
    liblmdb-dev libnghttp2-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Build liburing จาก source เพื่อให้ได้ recv_fixed / send_fixed
RUN git clone --depth 1 --branch liburing-2.6 \
      https://github.com/axboe/liburing.git /tmp/liburing \
 && cd /tmp/liburing \
 && ./configure --prefix=/usr/local \
 && make -j$(nproc) install

WORKDIR /app
COPY . .
# Build binary; in source mode also refresh lib/ so libonly stage stays fresh
RUN mkdir -p data && make clean && make \
 && if [ -f lib_dev/server.c ]; then make lib-pack; fi

# ── Lib Stage — export libcnext.a + headers from lib/ ─────────────────
# Via Makefile (recommended):
#   make pack-docker
# Or manually:
#   docker build --target=libonly -t cnext-lib .
#   docker create --name cnext-lib-tmp cnext-lib
#   docker cp cnext-lib-tmp:/libcnext.a lib/libcnext.a
#   docker cp cnext-lib-tmp:/include    lib/include
#   docker rm cnext-lib-tmp
FROM scratch AS libonly
COPY --from=builder /app/lib/libcnext.a /libcnext.a
COPY --from=builder /app/lib/include    /include

# ── Dev Stage — watch files, rebuild + restart on change ──────────────
# Used by `make dev` via compose.dev.yaml (bind-mounts src/, lib_dev/, etc.)
FROM builder AS dev
RUN apt-get update && apt-get install -y --no-install-recommends entr \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
EXPOSE 8080
# Outer loop re-runs `find` when new files appear (entr -d exits on new file)
CMD while true; do \
      find src lib_dev main.c Makefile tools -type f 2>/dev/null \
        | entr -dr sh -c 'make && ./server'; \
    done

# ── Runtime Stage ─────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    liblmdb0 libnghttp2-14 libcurl4 ca-certificates \
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

EXPOSE 8080

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
