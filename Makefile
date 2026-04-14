CC      = gcc
CFLAGS  = -O3 -march=native -Wall -Wextra -pthread -I/usr/local/include
LDFLAGS = -L/usr/local/lib -luring -llmdb
TARGET  = server
SRC     = main.c server.c db/hot.c db/cold.c db/db.c
DEPS    = server.h db/hot.h db/cold.h db/db.h

all: $(TARGET)

$(TARGET): $(SRC) $(DEPS)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

# สร้าง db directory สำหรับ LMDB files
db-dir:
	mkdir -p ./data

.PHONY: all clean db-dir
