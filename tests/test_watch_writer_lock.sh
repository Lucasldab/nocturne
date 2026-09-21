#!/usr/bin/env bash
# tests/test_watch_writer_lock.sh — write commands must work while watch runs.
#
# Regression guarded: `nocturned watch` used to hold the single-writer DB lock
# for its entire lifetime, so every short write command failed with rc=4
# (NOCT_EXIT_LOCK_BUSY) for as long as the service was up. On hearth that made
# `nocturned delete` unusable — the watcher is always running — and it is why
# nocturne-cycle-run has to stop the service, kill rogue watchers, poll the
# pidfile and retry three times before it can scan.
#
# The watcher now holds only a watcher-instance lock for its lifetime and takes
# the writer lock per scan, around the two places it actually writes.
#
# 1. Cold scan to build a DB with a real row.
# 2. Launch `nocturned watch`.
# 3. `nocturned delete <sha> --yes` must succeed, NOT exit 4.
# 4. The row must actually be gone (the delete did the work, not just exit 0).
# 5. A second watcher must still be refused — the instance lock still holds.

set -euo pipefail

BIN="${BIN:-build/nocturned}"
test -x "$BIN" || { echo "missing binary: $BIN" >&2; exit 1; }

NOCT_EXIT_LOCK_BUSY=4

TMP=$(mktemp -d -t nocturne-wlock-XXXXXX)
trap 'kill -TERM "${WPID:-}" 2>/dev/null || true; rm -rf "$TMP"' EXIT
export HOME="$TMP" XDG_DATA_HOME="$TMP" XDG_CACHE_HOME="$TMP" XDG_CONFIG_HOME="$TMP"

mkdir -p "$TMP/library/Artist/Album"
cp tests/fixtures/clean_id3v24.mp3 "$TMP/library/Artist/Album/01.mp3"

echo "==> [1/5] cold scan to populate the DB"
"$BIN" scan "$TMP/library" >/dev/null
DB_PATH="$TMP/nocturne/nocturne.db"
SHA=$(sqlite3 "$DB_PATH" "SELECT sha256 FROM tracks LIMIT 1;")
test -n "$SHA" || { echo "FAIL: no track row after scan" >&2; exit 1; }
echo "    track sha=${SHA:0:12}"

echo "==> [2/5] launch watch in background"
"$BIN" watch "$TMP/library" --debounce-ms 500 >"$TMP/watch.log" 2>&1 &
WPID=$!
sleep 1
if ! kill -0 "$WPID" 2>/dev/null; then
    echo "FAIL: watch exited prematurely" >&2; cat "$TMP/watch.log" >&2; exit 1
fi

echo "==> [3/5] delete against a LIVE watcher must not hit the writer lock"
rc=0
"$BIN" delete "$SHA" --yes >"$TMP/delete.log" 2>&1 || rc=$?
if [ "$rc" -eq "$NOCT_EXIT_LOCK_BUSY" ]; then
    echo "FAIL: delete got rc=4 (lock busy) — the watcher still holds the writer lock" >&2
    cat "$TMP/delete.log" >&2
    exit 1
fi
if [ "$rc" -ne 0 ]; then
    echo "FAIL: delete exited rc=$rc" >&2; cat "$TMP/delete.log" >&2; exit 1
fi

echo "==> [4/5] the row is actually gone (exit 0 alone proves nothing)"
left=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tracks WHERE sha256='$SHA';")
if [ "$left" != "0" ]; then
    echo "FAIL: delete exited 0 but the row is still present" >&2
    cat "$TMP/delete.log" >&2
    exit 1
fi

echo "==> [5/5] a second watcher is still refused (instance lock intact)"
rc2=0
"$BIN" watch "$TMP/library" --debounce-ms 500 >"$TMP/watch2.log" 2>&1 || rc2=$?
if [ "$rc2" -ne "$NOCT_EXIT_LOCK_BUSY" ]; then
    echo "FAIL: second watcher exited rc=$rc2, expected $NOCT_EXIT_LOCK_BUSY" >&2
    echo "      releasing the writer lock must not allow two watchers" >&2
    cat "$TMP/watch2.log" >&2
    exit 1
fi

kill -TERM "$WPID" 2>/dev/null || true
wait "$WPID" 2>/dev/null || true
echo "==> watch writer-lock PASSED"
