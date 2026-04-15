CC      = gcc
CFLAGS  = -O3 -march=native -Wall -Wextra -pthread -I. -Ilib -I/usr/local/include
LDFLAGS = -L/usr/local/lib -luring -llmdb -lnghttp2
TARGET  = server

# Core sources
CORE_SRC = main.c lib/server.c lib/h2.c lib/pages.c lib/static.c \
           lib/db/hot.c lib/db/cold.c lib/db/db.c

# src/**/page.c  → HTML pages  (เหมือน Next.js App Router)
# src/**/route.c → API routes  (เหมือน Next.js Route Handlers)
PAGE_SRC  = $(shell find src -name 'page.c'  2>/dev/null)
ROUTE_SRC = $(shell find src -name 'route.c' 2>/dev/null)

SRC  = $(CORE_SRC) $(PAGE_SRC) $(ROUTE_SRC)
DEPS = lib/server.h lib/pages.h lib/static.h \
       lib/db/hot.h lib/db/cold.h lib/db/db.h

all: $(TARGET)

$(TARGET): $(SRC) $(DEPS)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

db-dir:
	mkdir -p ./data

list-pages:
	@echo "Pages found:"
	@find src -name 'page.c' | sed 's|src||;s|/page.c||;s|^$$|/|' | sort

.PHONY: all clean db-dir list-pages
