CC          = gcc
CFLAGS_BASE = -O3 -march=native -Wall -Wextra -pthread -I. -I/usr/local/include
LDFLAGS     = -L/usr/local/lib -luring -llmdb -lnghttp2 -lcurl
TARGET      = server

# ── Mode detection ───────────────────────────────────────────────────
# source mode   : lib_dev/ exists (dev machine) → compile lib from .c
# consumer mode : lib_dev/ absent (EC2, CI, public clone) → link lib/libcnext.a
HAS_LIB_SRC := $(wildcard lib_dev/server.c)

ifneq ($(HAS_LIB_SRC),)
  MODE    = source
  CFLAGS  = $(CFLAGS_BASE) -Ilib_dev
  LIB_SRC = lib_dev/server.c lib_dev/h2.c lib_dev/pages.c lib_dev/static.c \
            lib_dev/fetch.c \
            lib_dev/db/hot.c lib_dev/db/cold.c lib_dev/db/db.c
  LIB_OBJ = $(LIB_SRC:.c=.o)
  LIB_A   = libcnext.a
  DEPS    = lib_dev/server.h lib_dev/pages.h lib_dev/static.h lib_dev/fetch.h lib_dev/route.h \
            lib_dev/db/hot.h lib_dev/db/cold.h lib_dev/db/db.h
else
  MODE   = consumer
  CFLAGS = $(CFLAGS_BASE) -Ilib/include
  LIB_A  = lib/libcnext.a
  DEPS   =
endif

# ── src/ → app code ──────────────────────────────────────────────────
# src/**/page.c    → HTML pages   (Next.js App Router style)
# src/**/route.c   → API routes   (Next.js Route Handlers style)
# src/**/action*.c → Server Actions (POST → redirect)
# src/**/*.cxn     → CXN templates (compile → *_cxn.c before build)
PAGE_SRC   = $(shell find src -name 'page.c'    2>/dev/null)
ROUTE_SRC  = $(shell find src -name 'route.c'   2>/dev/null)
ACTION_SRC = $(shell find src -name 'action*.c' 2>/dev/null)
CXN_SRC    = $(shell find src -name '*.cxn'     2>/dev/null)

# Build output — all cxnc-generated .c files live under .cnext/ (like .next in Next.js)
BUILD_DIR = .cnext
CXN_GEN   = $(patsubst %.cxn,$(BUILD_DIR)/%_cxn.c,$(CXN_SRC))

APP_SRC = main.c $(PAGE_SRC) $(ROUTE_SRC) $(ACTION_SRC) $(CXN_GEN)

all: $(CXN_GEN) $(LIB_A) $(TARGET)
	@echo "[build] mode=$(MODE) → $(TARGET)"

# src/foo/page.cxn → .cnext/src/foo/page_cxn.c
$(BUILD_DIR)/%_cxn.c: %.cxn tools/cxnc
	@mkdir -p $(dir $@)
	python3 tools/cxnc $< $@

ifeq ($(MODE),source)
# lib_dev/*.c → lib_dev/*.o
%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

# lib_dev/*.o → libcnext.a
$(LIB_A): $(LIB_OBJ)
	ar rcs $@ $^
	@echo "[lib] $(LIB_A) ready ($(words $(LIB_OBJ)) objects)"

# Pack lib/ for distribution (copy built libcnext.a + public headers)
lib-pack: $(LIB_A)
	rm -rf lib/include
	mkdir -p lib/include/db
	cp $(LIB_A) lib/libcnext.a
	cp lib_dev/*.h lib/include/
	cp lib_dev/db/*.h lib/include/db/
	@echo "[lib] packed → lib/libcnext.a + include/"
endif

# Refresh lib/ via Docker — use on macOS where liburing can't compile natively
pack-docker:
	docker build --target=builder -t cnext-pack-builder .
	docker rm -f cnext-pack 2>/dev/null || true
	docker create --name cnext-pack cnext-pack-builder
	mkdir -p lib
	docker cp cnext-pack:/app/lib/libcnext.a lib/libcnext.a
	rm -rf lib/include
	docker cp cnext-pack:/app/lib/include lib/include
	docker rm cnext-pack
	@echo "[pack] lib/ refreshed via docker — commit and push"

# link final binary
$(TARGET): $(APP_SRC) $(LIB_A) $(DEPS)
	$(CC) $(CFLAGS) $(APP_SRC) $(LIB_A) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
ifeq ($(MODE),source)
	rm -f $(LIB_OBJ) $(LIB_A)
endif

# Dev mode — docker compose + entr: rebuild + restart on file change
dev:
	docker compose -f compose.dev.yaml up --build

dev-down:
	docker compose -f compose.dev.yaml down

db-dir:
	mkdir -p ./data

list-pages:
	@echo "Pages (page.c):"; find src -name 'page.c'  | sed 's|src||;s|/page.c||;s|^$$|/|'  | sort
	@echo "Pages (.cxn):";   find src -name '*.cxn'   | sed 's|src||;s|\.cxn$$||' | sort

.PHONY: all clean db-dir list-pages lib-pack pack-docker dev dev-down
