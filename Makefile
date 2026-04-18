CC          = gcc
CFLAGS_BASE = -O3 -march=native -Wall -Wextra -pthread -I. -I/usr/local/include
LDFLAGS     = -L/usr/local/lib -luring -llmdb -lnghttp2 -lcurl
TARGET      = server

# ── Mode detection ───────────────────────────────────────────────────
# source mode   : lib/ exists (dev machine) → compile lib from .c
# consumer mode : lib/ absent (EC2, CI, public clone) → link plib/libcnext.a
HAS_LIB_SRC := $(wildcard lib/server.c)

ifneq ($(HAS_LIB_SRC),)
  MODE    = source
  CFLAGS  = $(CFLAGS_BASE) -Ilib
  LIB_SRC = lib/server.c lib/h2.c lib/pages.c lib/static.c \
            lib/fetch.c \
            lib/db/hot.c lib/db/cold.c lib/db/db.c
  LIB_OBJ = $(LIB_SRC:.c=.o)
  LIB_A   = libcnext.a
  DEPS    = lib/server.h lib/pages.h lib/static.h lib/fetch.h lib/route.h \
            lib/db/hot.h lib/db/cold.h lib/db/db.h
else
  MODE   = consumer
  CFLAGS = $(CFLAGS_BASE) -Iplib/include
  LIB_A  = plib/libcnext.a
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
CXN_GEN    = $(CXN_SRC:.cxn=_cxn.c)

APP_SRC = main.c $(PAGE_SRC) $(ROUTE_SRC) $(ACTION_SRC) $(CXN_GEN)

all: $(CXN_GEN) $(LIB_A) $(TARGET)
	@echo "[build] mode=$(MODE) → $(TARGET)"

# .cxn → _cxn.c
%_cxn.c: %.cxn tools/cxnc
	python3 tools/cxnc $< $@

ifeq ($(MODE),source)
# lib/*.c → lib/*.o
%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

# lib/*.o → libcnext.a
$(LIB_A): $(LIB_OBJ)
	ar rcs $@ $^
	@echo "[lib] $(LIB_A) ready ($(words $(LIB_OBJ)) objects)"

# Pack plib/ for distribution (copy built libcnext.a + public headers)
plib-pack: $(LIB_A)
	rm -rf plib/include
	mkdir -p plib/include/db
	cp $(LIB_A) plib/libcnext.a
	cp lib/*.h plib/include/
	cp lib/db/*.h plib/include/db/
	@echo "[plib] packed → plib/libcnext.a + include/"
endif

# Refresh plib/ via Docker — use on macOS where liburing can't compile natively
pack-docker:
	docker build --target=builder -t cnext-pack-builder .
	docker rm -f cnext-pack 2>/dev/null || true
	docker create --name cnext-pack cnext-pack-builder
	mkdir -p plib
	docker cp cnext-pack:/app/plib/libcnext.a plib/libcnext.a
	rm -rf plib/include
	docker cp cnext-pack:/app/plib/include plib/include
	docker rm cnext-pack
	@echo "[pack] plib/ refreshed via docker — commit and push"

# link final binary
$(TARGET): $(APP_SRC) $(LIB_A) $(DEPS)
	$(CC) $(CFLAGS) $(APP_SRC) $(LIB_A) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(CXN_GEN)
ifeq ($(MODE),source)
	rm -f $(LIB_OBJ) $(LIB_A)
endif

db-dir:
	mkdir -p ./data

list-pages:
	@echo "Pages (page.c):"; find src -name 'page.c'  | sed 's|src||;s|/page.c||;s|^$$|/|'  | sort
	@echo "Pages (.cxn):";   find src -name '*.cxn'   | sed 's|src||;s|\.cxn$$||' | sort

.PHONY: all clean db-dir list-pages plib-pack pack-docker
