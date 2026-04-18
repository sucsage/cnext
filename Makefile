CC      = gcc
CFLAGS  = -O3 -march=native -Wall -Wextra -pthread -I. -Ilib -I/usr/local/include
LDFLAGS = -L/usr/local/lib -luring -llmdb -lnghttp2 -lcurl
TARGET  = server
LIB_A   = libcnext.a

# ── lib/ → static library ────────────────────────────────────────────
LIB_SRC = lib/server.c lib/h2.c lib/pages.c lib/static.c \
          lib/fetch.c \
          lib/db/hot.c lib/db/cold.c lib/db/db.c
LIB_OBJ = $(LIB_SRC:.c=.o)

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

DEPS = lib/server.h lib/pages.h lib/static.h lib/fetch.h \
       lib/db/hot.h lib/db/cold.h lib/db/db.h

all: $(CXN_GEN) $(LIB_A) $(TARGET)

# .cxn → _cxn.c
%_cxn.c: %.cxn tools/cxnc
	python3 tools/cxnc $< $@

# lib/*.c → lib/*.o
%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

# lib/*.o → libcnext.a
$(LIB_A): $(LIB_OBJ)
	ar rcs $@ $^
	@echo "[lib] $(LIB_A) ready ($(words $(LIB_OBJ)) objects)"

# app + libcnext.a → server
$(TARGET): $(APP_SRC) $(LIB_A) $(DEPS)
	$(CC) $(CFLAGS) $(APP_SRC) $(LIB_A) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(CXN_GEN) $(LIB_OBJ) $(LIB_A)

db-dir:
	mkdir -p ./data

list-pages:
	@echo "Pages (page.c):"; find src -name 'page.c'  | sed 's|src||;s|/page.c||;s|^$$|/|'  | sort
	@echo "Pages (.cxn):";   find src -name '*.cxn'   | sed 's|src||;s|\.cxn$$||' | sort

.PHONY: all clean db-dir list-pages
