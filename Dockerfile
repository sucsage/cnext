# ── Build Stage ──────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git ca-certificates liblmdb-dev \
    && rm -rf /var/lib/apt/lists/*

# Build liburing จาก source เพื่อให้ได้ recv_fixed / send_fixed
RUN git clone --depth 1 --branch liburing-2.6 \
      https://github.com/axboe/liburing.git /tmp/liburing \
 && cd /tmp/liburing \
 && ./configure --prefix=/usr/local \
 && make -j$(nproc) install

WORKDIR /app
COPY . .
RUN mkdir -p data && make clean && make

# ── Runtime Stage ─────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    liblmdb0 \
    && rm -rf /var/lib/apt/lists/*

# copy liburing .so จาก builder (ไม่ต้อง build ซ้ำ)
COPY --from=builder /usr/local/lib/liburing.so.2   /usr/local/lib/
COPY --from=builder /usr/local/lib/liburing.so.2.6 /usr/local/lib/
RUN ldconfig

WORKDIR /app
COPY --from=builder /app/server .
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
