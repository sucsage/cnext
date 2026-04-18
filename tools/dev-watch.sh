#!/bin/sh
# Polling watcher for dev mode
# Docker Desktop on macOS uses virtio-fs where inotify events from the host
# don't propagate into the container — so we poll mtimes every second.
set -u

WATCH="src lib main.c Makefile public"
MARKER=/tmp/.cnext-dev-last
SERVER_PID=

# Tell the framework we're in dev mode — enables /__dev/ping and the
# auto-reload script injection (see lib_dev/pages.c).
export CNEXT_DEV=1

start() {
  if make; then
    ./server &
    SERVER_PID=$!
    echo "[dev] server started (pid=$SERVER_PID)"
  else
    echo "[dev] build failed — waiting for next change"
    SERVER_PID=
  fi
}

stop() {
  if [ -n "$SERVER_PID" ]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
  fi
}

trap 'stop; exit 0' INT TERM

touch "$MARKER"
start

while true; do
  CHANGED=$(find $WATCH -type f -newer "$MARKER" 2>/dev/null | head -5)
  if [ -n "$CHANGED" ]; then
    echo "[dev] change detected:"
    echo "$CHANGED" | sed 's/^/  /'
    touch "$MARKER"
    stop
    start
  fi
  sleep 1
done
