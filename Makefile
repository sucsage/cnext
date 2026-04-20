CC          = gcc
CFLAGS      = -O3 -march=native -Wall -Wextra -pthread -I. -I.cnext -Ilib/include -I/usr/local/include
LDFLAGS     = -L/usr/local/lib -luring -llmdb -lnghttp2 -lcurl -lssl -lcrypto
TARGET      = server
LIB_A       = lib/libcnext.a

# ── src/ → app code ──────────────────────────────────────────────────
# src/**/page.c    → HTML pages          (Next.js App Router style)
# src/**/page.cxn  → CXN templates       (compiled by tools/cxnc)
# src/**/route.c   → API routes          (Next.js Route Handlers)
# src/**/action*.c → Server Actions      (POST → redirect)
PAGE_SRC   = $(shell find src -name 'page.c'    2>/dev/null)
ROUTE_SRC  = $(shell find src -name 'route.c'   2>/dev/null)
ACTION_SRC = $(shell find src -name 'action*.c' 2>/dev/null)
CXN_SRC    = $(shell find src -name '*.cxn'     2>/dev/null)

BUILD_DIR = .cnext
CXN_GEN   = $(patsubst %.cxn,$(BUILD_DIR)/%_cxn.c,$(CXN_SRC))
APP_SRC   = main.c $(PAGE_SRC) $(ROUTE_SRC) $(ACTION_SRC) $(CXN_GEN)

UNAME_S := $(shell uname -s)

# =====================================================================
# Build rules
# =====================================================================
all: $(CXN_GEN) $(TARGET)

$(BUILD_DIR)/%_cxn.c: %.cxn tools/cxnc
	@mkdir -p $(dir $@)
	python3 tools/cxnc $< $@

$(TARGET): $(APP_SRC) $(LIB_A)
	$(CC) $(CFLAGS) $(APP_SRC) $(LIB_A) -o $(TARGET) $(LDFLAGS)

# =====================================================================
# Docker targets — work on macOS / Windows / Linux
# =====================================================================

# Live-reload dev server — edit files and the browser auto-refreshes
dev:
	@mkdir -p certs
	docker compose -f compose.dev.yaml up --build

dev-down:
	docker compose -f compose.dev.yaml down

# Production build + run
start:
	docker build -t cnext .
	docker run --rm -p 8080:8080 -p 8443:8443 \
		-v $(PWD)/data:/app/data \
		-v $(PWD)/certs:/app/certs:ro \
		--security-opt seccomp=unconfined \
		--cap-add SYS_NICE \
		--ulimit nofile=65536:65536 \
		--ulimit memlock=-1 \
		cnext

# ── HTTPS — drop cert.pem + key.pem into certs/ ──────────────────────
gen-cert:
	sh tools/gen-dev-cert.sh

# =====================================================================
# Native Linux build — no Docker
# Prereqs: build-essential python3 liblmdb-dev libnghttp2-dev
#          libcurl4-openssl-dev + liburing 2.6+ (see README)
# =====================================================================
native:
ifneq ($(UNAME_S),Linux)
	@echo "[native] $(UNAME_S) is not supported — io_uring is Linux-only."
	@echo "[native] macOS / Windows → use: make dev  (runs in Docker)"
	@exit 1
else
	@$(MAKE) --no-print-directory _check-deps
	@$(MAKE) --no-print-directory all
	@echo "[native] done → run with: ./server"
endif

_check-deps:
	@missing=""; \
	command -v $(CC)   >/dev/null 2>&1 || missing="$$missing gcc"; \
	command -v python3 >/dev/null 2>&1 || missing="$$missing python3"; \
	[ -f /usr/include/lmdb.h ]            || missing="$$missing liblmdb-dev"; \
	[ -f /usr/include/nghttp2/nghttp2.h ] || missing="$$missing libnghttp2-dev"; \
	[ -f /usr/include/curl/curl.h ] || [ -f /usr/include/x86_64-linux-gnu/curl/curl.h ] \
	                                      || missing="$$missing libcurl4-openssl-dev"; \
	[ -f /usr/include/openssl/ssl.h ]     || missing="$$missing libssl-dev"; \
	[ -f /usr/include/liburing.h ] || [ -f /usr/local/include/liburing.h ] \
	                                      || missing="$$missing liburing"; \
	if [ -n "$$missing" ]; then \
	  echo "[native] missing deps:$$missing"; \
	  echo "[native] install with:"; \
	  echo "    sudo apt install build-essential python3 liblmdb-dev libnghttp2-dev libcurl4-openssl-dev libssl-dev"; \
	  echo "    # liburing 2.6+ — build from source if your distro lacks it:"; \
	  echo "    #   git clone --depth 1 --branch liburing-2.6 https://github.com/axboe/liburing.git"; \
	  echo "    #   cd liburing && ./configure --prefix=/usr/local && make && sudo make install && sudo ldconfig"; \
	  exit 1; \
	fi

# =====================================================================
# Housekeeping
# =====================================================================
clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)

list-pages:
	@echo "Pages (page.c):"; find src -name 'page.c'  | sed 's|src||;s|/page.c||;s|^$$|/|' | sort
	@echo "Pages (.cxn):";   find src -name '*.cxn'   | sed 's|src||;s|\.cxn$$||' | sort

help:
	@echo "cnext — available targets:"
	@echo ""
	@echo "  make dev         live-reload dev server   (Docker, any OS)"
	@echo "  make dev-down    stop the dev container"
	@echo "  make start       production build + run   (Docker, any OS)"
	@echo "  make native      native build             (Linux only, no Docker)"
	@echo "  make gen-cert    generate self-signed dev cert into certs/"
	@echo "  make clean       remove build artifacts"
	@echo "  make list-pages  show discovered routes"

.PHONY: all clean list-pages dev dev-down start native _check-deps gen-cert help
