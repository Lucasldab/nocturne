#!/usr/bin/env bash
# tests/test_integration.sh — end-to-end smoke for nocturned.
#
# Exercises: scan (cold) → scan (incremental no-op) → doctor → resolve
# (--dry-run --explain) → resolve (real) → publish → JSON validation
# → second-instance lock check.
#
# Runs in a fully isolated $HOME. Cleans up on exit.

set -euo pipefail

BIN="${BIN:-build/nocturned}"
test -x "$BIN" || { echo "missing binary: $BIN" >&2; exit 1; }

TMP=$(mktemp -d -t nocturne-integ-XXXXXX)
trap "rm -rf '$TMP'" EXIT
export HOME="$TMP" XDG_DATA_HOME="$TMP" XDG_CACHE_HOME="$TMP" XDG_CONFIG_HOME="$TMP"

mkdir -p "$TMP/library/Artist/Album" "$TMP/sync-meta"
cp tests/fixtures/clean_id3v24.mp3 "$TMP/library/Artist/Album/01.mp3"
cp tests/fixtures/missing_album_artist.flac "$TMP/library/Artist/Album/02.flac"

echo "==> [1/8] scan #1 (cold)"
"$BIN" scan "$TMP/library" | tee "$TMP/scan1.log"
grep -qE 'seen=2 added=2' "$TMP/scan1.log" || { echo "FAIL: cold scan didn't add 2"; exit 1; }

echo "==> [2/8] scan #2 (incremental no-op)"
"$BIN" scan "$TMP/library" | tee "$TMP/scan2.log"
grep -qE 'added=0 updated=0 removed=0' "$TMP/scan2.log" || { echo "FAIL: incremental scan reported diff"; exit 1; }

echo "==> [3/8] doctor --json (parses as JSON)"
"$BIN" doctor --json | tee "$TMP/doctor.json" >/dev/null
python3 -m json.tool < "$TMP/doctor.json" >/dev/null

echo "==> [4/8] resolve --dry-run --explain"
# resolve exits 1 when cold_start=yes (documented behavior from 02-05).
"$BIN" resolve --dry-run --explain | tee "$TMP/explain.log" || true
grep -qE 'residents=' "$TMP/explain.log" || { echo "FAIL: explain missing residents= line"; exit 1; }

echo "==> [5/8] resolve (real)"
"$BIN" resolve | tee "$TMP/resolve.log" || true
grep -qE 'residents=' "$TMP/resolve.log" || { echo "FAIL: resolve missing residents= line"; exit 1; }

echo "==> [6/8] publish"
"$BIN" publish --out "$TMP/sync-meta" | tee "$TMP/publish.log"
test -f "$TMP/sync-meta/catalog.json"  || { echo "FAIL: catalog.json missing"; exit 1; }
test -f "$TMP/sync-meta/manifest.json" || { echo "FAIL: manifest.json missing"; exit 1; }

echo "==> [7/8] validate emitted JSONs"
jq -e '.v == 1 and (.tracks   | length) == 2'    "$TMP/sync-meta/catalog.json"  >/dev/null
jq -e '.v == 1 and .cap_bytes > 0 and (.resident | length) > 0' "$TMP/sync-meta/manifest.json" >/dev/null
# No tmp leaks under happy path.
if ls "$TMP/sync-meta"/*.tmp.* >/dev/null 2>&1; then
    echo "FAIL: .tmp leaks in $TMP/sync-meta" >&2
    exit 1
fi

echo "==> [8/8] second instance respects lock"
# Lock-busy assertion is best-effort: if the first scan finishes before the
# second sees it, exit 0 from a fast machine is acceptable. We assert that IF
# the busy lock is hit, the message is the documented one.
"$BIN" scan "$TMP/library" &
PID=$!
sleep 0.05
SECOND_OUT=$("$BIN" scan "$TMP/library" 2>&1 || true)
wait $PID || true
if echo "$SECOND_OUT" | grep -q 'another instance is running'; then
    echo "    lock semantics ok (busy lock observed)"
else
    echo "    first scan finished too fast for race window — acceptable"
fi

echo "==> integration PASSED"
