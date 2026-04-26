#!/usr/bin/env bash
# tests/test_e2e_watch.sh — end-to-end watch loop check.
#
# 1. Initialise empty library + DB.
# 2. Launch `nocturned watch` in the background (short debounce).
# 3. Copy a fixture audio file into the watched tree.
# 4. Wait for the row to appear in the DB (poll up to 10s).
# 5. SIGTERM the watcher and confirm clean shutdown.

set -euo pipefail

BIN="${BIN:-build/nocturned}"
test -x "$BIN" || { echo "missing binary: $BIN" >&2; exit 1; }

TMP=$(mktemp -d -t nocturne-watch-XXXXXX)
trap 'kill -TERM "${WPID:-}" 2>/dev/null || true; rm -rf "$TMP"' EXIT
export HOME="$TMP" XDG_DATA_HOME="$TMP" XDG_CACHE_HOME="$TMP" XDG_CONFIG_HOME="$TMP"

mkdir -p "$TMP/library/Artist/Album"

echo "==> [1/5] initialise empty DB via cold scan"
"$BIN" scan "$TMP/library" >/dev/null

echo "==> [2/5] launch watch in background (debounce 500ms)"
"$BIN" watch "$TMP/library" --debounce-ms 500 >"$TMP/watch.log" 2>&1 &
WPID=$!
sleep 1
if ! kill -0 "$WPID" 2>/dev/null; then
    echo "FAIL: watch process exited prematurely" >&2
    cat "$TMP/watch.log" >&2
    exit 1
fi

echo "==> [3/5] copy fixture into watched tree"
cp tests/fixtures/clean_id3v24.mp3 "$TMP/library/Artist/Album/03.mp3"

echo "==> [4/5] poll DB for the new row (up to 10s)"
DB_PATH="$TMP/nocturne/nocturne.db"
found=0
for i in $(seq 1 20); do
    if [ -f "$DB_PATH" ] \
       && sqlite3 "$DB_PATH" "SELECT path FROM tracks WHERE path LIKE '%03.mp3';" 2>/dev/null \
          | grep -q '03.mp3'; then
        found=1
        break
    fi
    sleep 0.5
done
if [ "$found" -ne 1 ]; then
    echo "FAIL: row for 03.mp3 never appeared in DB after 10s" >&2
    cat "$TMP/watch.log" >&2
    exit 1
fi

echo "==> [5/5] SIGTERM the watcher"
kill -TERM "$WPID"
# Wait up to 5s for clean shutdown.
for i in $(seq 1 10); do
    if ! kill -0 "$WPID" 2>/dev/null; then
        break
    fi
    sleep 0.5
done
if kill -0 "$WPID" 2>/dev/null; then
    echo "FAIL: watch did not exit within 5s of SIGTERM" >&2
    kill -KILL "$WPID" 2>/dev/null || true
    exit 1
fi
wait "$WPID" 2>/dev/null || true

echo "==> e2e watch PASSED"
