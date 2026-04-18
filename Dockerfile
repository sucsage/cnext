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
# Build binary; in source mode also refresh plib/ so libonly stage stays fresh
RUN mkdir -p data && make clean && make \
 && if [ -f lib/server.c ]; then make plib-pack; fi

# ── Lib Stage — export libcnext.a + headers from plib/ ────────────────
# Recommended (buildx):
#   docker buildx build --target=libonly -o type=local,dest=plib .
# Or via classic build:
#   docker build --target=libonly -t cnext-lib .
#   docker create --name _lib cnext-lib
#   docker cp _lib:/dist/. ./plib && docker rm _lib
FROM scratch AS libonly
COPY --from=builder /app/plib/libcnext.a /libcnext.a
COPY --from=builder /app/plib/include    /include

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
