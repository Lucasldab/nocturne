#!/usr/bin/env bash
# tests/test_integration_ingest.sh — end-to-end Phase 7 loop closure.
#
# Proves INGEST-04: scan a small library, write synthetic phone JSONL,
# ingest it, run resolve + publish, observe the resulting manifest
# reflects the new stats (top_played / loved / manual_pins buckets).
#
# Hermetic — runs entirely under a tmp $HOME / XDG_*; never touches
# the user's daemon DB.

set -euo pipefail

BIN="${BIN:-build/nocturned}"
test -x "$BIN" || { echo "missing binary: $BIN" >&2; exit 1; }

WORK=$(mktemp -d -t nocturne-int-ingest-XXXXXX)
trap "rm -rf '$WORK'" EXIT

LIB="$WORK/library"
META="$WORK/meta"
DB="$WORK/nocturne/nocturne.db"
mkdir -p "$LIB/Artist1/Album1" "$LIB/Artist2/Album2" "$LIB/Artist3/AlbumA" \
         "$META/stats" "$WORK/nocturne"

export HOME="$WORK"
export XDG_DATA_HOME="$WORK"
export XDG_CACHE_HOME="$WORK"
export XDG_CONFIG_HOME="$WORK"

# 1. Build a small library by copying existing fixture audio files.
#    Each track must have a unique sha256 (sha256 is over audio bytes,
#    so different fixtures suffice). We need >= 3 tracks.
cp tests/fixtures/clean_id3v24.mp3        "$LIB/Artist1/Album1/01.mp3"
cp tests/fixtures/clean_all.flac          "$LIB/Artist2/Album2/01.flac"
cp tests/fixtures/missing_album_artist.flac "$LIB/Artist3/AlbumA/01.flac"

echo "==> [1/9] scan library"
"$BIN" scan "$LIB" | tee "$WORK/scan.log"
TRACK_COUNT=$(sqlite3 "$DB" "SELECT COUNT(*) FROM tracks;")
[ "$TRACK_COUNT" -ge 3 ] || { echo "FAIL: only $TRACK_COUNT tracks scanned, need 3+"; exit 1; }
echo "    tracks scanned: $TRACK_COUNT"

# 2. Pull three sha256s out of the DB. Sort by sha256 ASC for
#    determinism so SHA1 / SHA2 / SHA3 are stable across runs.
SHA1=$(sqlite3 "$DB" "SELECT sha256 FROM tracks ORDER BY sha256 LIMIT 1;")
SHA2=$(sqlite3 "$DB" "SELECT sha256 FROM tracks ORDER BY sha256 LIMIT 1 OFFSET 1;")
SHA3=$(sqlite3 "$DB" "SELECT sha256 FROM tracks ORDER BY sha256 LIMIT 1 OFFSET 2;")
echo "    SHA1=${SHA1:0:12} SHA2=${SHA2:0:12} SHA3=${SHA3:0:12}"

# 3. Snapshot the manifest BEFORE ingest. resolve exits 1 in cold-
#    start mode (documented; not a regression).
echo "==> [2/9] resolve + publish (pre-ingest baseline)"
"$BIN" resolve >/dev/null 2>&1 || true
"$BIN" publish --out "$META" >/dev/null
test -f "$META/manifest.json" || { echo "FAIL: pre-ingest manifest not written"; exit 1; }
cp "$META/manifest.json" "$WORK/manifest-before.json"
echo "    manifest-before.json snapshotted"

# 4. Generate synthetic JSONL: 50 plays of SHA1, 1 like for SHA2,
#    1 pin for SHA3. ts values are recent so the time-windowed
#    top_played bucket picks them up.
NOW_MS=$(date +%s%3N)

# 50 plays of SHA1 over the last hour (one per minute).
{
    for i in $(seq 1 50); do
        TS=$((NOW_MS - i * 60000))
        printf '{"v":1,"ts":%s,"kind":"play","track":"%s","played_ms":180000,"duration_ms":180000}\n' \
            "$TS" "$SHA1"
    done
} > "$META/stats/phone-T1.jsonl"

# Like SHA2 (track).
printf '{"v":1,"ts":%s,"unit":"track","id":"%s","liked":true}\n' \
    "$NOW_MS" "$SHA2" > "$META/likes-phone-T1.jsonl"

# Pin SHA3 (track).
printf '{"v":1,"ts":%s,"unit":"track","id":"%s","pinned":true}\n' \
    "$NOW_MS" "$SHA3" > "$META/pins-phone-T1.jsonl"

echo "==> [3/9] ingest #1"
"$BIN" ingest --meta-dir "$META" 2>&1 | tee "$WORK/ingest1.log"
grep -qE 'plays=50' "$WORK/ingest1.log" || { echo "FAIL: plays not ingested"; exit 1; }
grep -qE 'likes=1'  "$WORK/ingest1.log" || { echo "FAIL: like not ingested"; exit 1; }
grep -qE 'pins=1'   "$WORK/ingest1.log" || { echo "FAIL: pin not ingested"; exit 1; }

echo "==> [4/9] ingest #2 (idempotency: INGEST-02)"
"$BIN" ingest --meta-dir "$META" 2>&1 | tee "$WORK/ingest2.log"
grep -qE 'plays=0 likes=0 pins=0 offsets_advanced=0' "$WORK/ingest2.log" \
    || { echo "FAIL: re-run not idempotent"; exit 1; }

echo "==> [5/9] resolve + publish (post-ingest)"
"$BIN" resolve | tee "$WORK/resolve.log"
"$BIN" publish --out "$META" >/dev/null
cp "$META/manifest.json" "$WORK/manifest-after.json"

# 5. Manifest assertions.
echo "==> [6/9] manifest changed (before != after)"
if cmp -s "$WORK/manifest-before.json" "$WORK/manifest-after.json"; then
    echo "FAIL: manifest unchanged after ingest"
    exit 1
fi
echo "    manifest differs from baseline"

echo "==> [7/9] resident set contains SHA1 / SHA2 / SHA3"
jq -e --arg s "$SHA1" '.resident[] | select(.id == $s)' "$WORK/manifest-after.json" >/dev/null \
    || { echo "FAIL: SHA1 not in resident set after 50 plays"; exit 1; }
jq -e --arg s "$SHA2" '.resident[] | select(.id == $s)' "$WORK/manifest-after.json" >/dev/null \
    || { echo "FAIL: SHA2 (liked) not in resident set"; exit 1; }
jq -e --arg s "$SHA3" '.resident[] | select(.id == $s)' "$WORK/manifest-after.json" >/dev/null \
    || { echo "FAIL: SHA3 (pinned) not in resident set"; exit 1; }
echo "    all 3 sha256s in resident set"

echo "==> [8/9] bucket attribution"
# SHA1 should carry top_played; SHA2 should carry loved; SHA3 should
# carry manual_pins. recent_adds may also attach to all three (every
# scanned track is a recent add); that's fine.
jq -e --arg s "$SHA1" '.resident[] | select(.id == $s) | .buckets | index("top_played")' \
    "$WORK/manifest-after.json" >/dev/null \
    || { echo "FAIL: SHA1 not attributed to top_played"; exit 1; }
jq -e --arg s "$SHA2" '.resident[] | select(.id == $s) | .buckets | index("loved")' \
    "$WORK/manifest-after.json" >/dev/null \
    || { echo "FAIL: SHA2 not attributed to loved"; exit 1; }
jq -e --arg s "$SHA3" '.resident[] | select(.id == $s) | .buckets | index("manual_pins")' \
    "$WORK/manifest-after.json" >/dev/null \
    || { echo "FAIL: SHA3 not attributed to manual_pins"; exit 1; }
echo "    SHA1∈top_played, SHA2∈loved, SHA3∈manual_pins"

echo "==> [9/9] ingest_offsets persisted (sanity)"
OFFROWS=$(sqlite3 "$DB" "SELECT COUNT(*) FROM ingest_offsets;")
[ "$OFFROWS" = "3" ] || { echo "FAIL: 3 ingest_offsets rows expected, got $OFFROWS"; exit 1; }

echo "test_integration_ingest: OK"
