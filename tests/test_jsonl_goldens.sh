#!/usr/bin/env bash
# tests/test_jsonl_goldens.sh — bridge between docs/jsonl-spec.md and
# the runtime ingester. Lays out the three byte-frozen goldens
# (tests/fixtures/jsonl-goldens/) under a tmp meta_dir, runs `nocturned
# ingest`, and asserts the resulting DB state matches the spec's stated
# semantics: 3 plays, 1 like row (LWW collapsed unlike onto like), 1
# pinned-track row + 1 pin row for the LWW-resolved album, idempotent
# re-run zeroes everything.
#
# Skip semantics: none — the goldens + the ingester are always present.

set -euo pipefail

BIN="${BIN:-build/nocturned}"
test -x "$BIN" || { echo "missing binary: $BIN" >&2; exit 1; }

WORK=$(mktemp -d -t nocturne-goldens-XXXXXX)
trap "rm -rf '$WORK'" EXIT

META="$WORK/meta"
DB="$WORK/nocturne/nocturne.db"
mkdir -p "$META/stats" "$WORK/nocturne"

# Hermetic environment: redirect every XDG dir under $WORK so the test
# never touches the user's daemon DB.
export HOME="$WORK"
export XDG_DATA_HOME="$WORK"
export XDG_CACHE_HOME="$WORK"
export XDG_CONFIG_HOME="$WORK"

# 1. Lay out the goldens.
cp tests/fixtures/jsonl-goldens/stats-golden.jsonl "$META/stats/phone-G0LD.jsonl"
cp tests/fixtures/jsonl-goldens/likes-golden.jsonl "$META/likes-phone-G0LD.jsonl"
cp tests/fixtures/jsonl-goldens/pins-golden.jsonl  "$META/pins-phone-G0LD.jsonl"

# 2. Pre-seed the tracks table so plays foreign-key resolves. The
#    ingester runs migrations on first open, so we touch the binary
#    once with a zero-effect command (`doctor --json`) to materialise
#    the schema, then INSERT via sqlite3.
"$BIN" doctor --json >/dev/null 2>&1 || true
test -f "$DB" || { echo "FAIL: DB not created at $DB"; exit 1; }

SHA_A="9a3f4e1b2c5d6e7f8091a2b3c4d5e6f70819f0e1d2c3b4a5968778695a4b3c2d"
SHA_B="b1c2d3e4f50617283940a1b2c3d4e5f60718293a4b5c6d7e8f900112233445dd"

sqlite3 "$DB" <<EOF
INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, tags_status, date_added, last_seen_at)
VALUES ('$SHA_A', '/tmp/golden-a.mp3', 0, 1024, 'ok',
        '2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z');
INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, tags_status, date_added, last_seen_at)
VALUES ('$SHA_B', '/tmp/golden-b.mp3', 0, 1024, 'ok',
        '2026-04-26T00:00:00Z', '2026-04-26T00:00:00Z');
EOF

# 3. Run ingest #1 against the goldens.
"$BIN" ingest --meta-dir "$META" 2>&1 | tee "$WORK/ingest1.log"
grep -qE 'files=3'                "$WORK/ingest1.log" || { echo "FAIL: files != 3"; exit 1; }
grep -qE 'plays=3'                "$WORK/ingest1.log" || { echo "FAIL: plays != 3"; exit 1; }
grep -qE 'likes=3'                "$WORK/ingest1.log" || { echo "FAIL: likes != 3 (3 events processed)"; exit 1; }
grep -qE 'pins=3'                 "$WORK/ingest1.log" || { echo "FAIL: pins != 3"; exit 1; }
grep -qE 'offsets_advanced=3'     "$WORK/ingest1.log" || { echo "FAIL: offsets_advanced != 3"; exit 1; }
grep -qE 'parse_errors=0'         "$WORK/ingest1.log" || { echo "FAIL: parse_errors != 0"; exit 1; }
grep -qE 'oversize_lines=0'       "$WORK/ingest1.log" || { echo "FAIL: oversize_lines != 0"; exit 1; }

# 4. DB state assertions.
PLAYS=$(sqlite3 "$DB" "SELECT COUNT(*) FROM plays;")
[ "$PLAYS" = "3" ] || { echo "FAIL: plays count == 3 expected, got $PLAYS"; exit 1; }

# Likes: 3 events, but track LWW collapses 2 events into 1 row, plus 1
# album row → 2 rows total.
LIKE_ROWS=$(sqlite3 "$DB" "SELECT COUNT(*) FROM likes;")
[ "$LIKE_ROWS" = "2" ] || { echo "FAIL: likes row count == 2 expected, got $LIKE_ROWS"; exit 1; }

# Track-likes LWW: ts=200 like → ts=400 unlike → final liked=0.
LIKED_TRACK=$(sqlite3 "$DB" "SELECT liked FROM likes WHERE unit='track' AND id='$SHA_A';")
[ "$LIKED_TRACK" = "0" ] || { echo "FAIL: track-like LWW: expected 0 (unlike won), got $LIKED_TRACK"; exit 1; }

# Album-like row: ts=500 like, no later unlike → final liked=1.
LIKED_ALBUM=$(sqlite3 "$DB" "SELECT liked FROM likes WHERE unit='album' AND id='alb_abcd1234';")
[ "$LIKED_ALBUM" = "1" ] || { echo "FAIL: album-like: expected 1, got $LIKED_ALBUM"; exit 1; }

# Pins: 3 events on (track:SHA_B, album:alb_abcd1234) — track has 1
# pin event, album has pin@500 + unpin@700 (LWW resolves to 0).
PIN_ROWS=$(sqlite3 "$DB" "SELECT COUNT(*) FROM pins;")
[ "$PIN_ROWS" = "2" ] || { echo "FAIL: pins row count == 2 expected, got $PIN_ROWS"; exit 1; }

PINNED_ALBUM=$(sqlite3 "$DB" "SELECT pinned FROM pins WHERE unit='album' AND id='alb_abcd1234';")
[ "$PINNED_ALBUM" = "0" ] || { echo "FAIL: album-pin LWW: expected 0 (unpin won), got $PINNED_ALBUM"; exit 1; }

PINNED_TRACK=$(sqlite3 "$DB" "SELECT pinned FROM pins WHERE unit='track' AND id='$SHA_B';")
[ "$PINNED_TRACK" = "1" ] || { echo "FAIL: track-pin: expected 1, got $PINNED_TRACK"; exit 1; }

# Ingest_offsets: each file's persisted offset == its on-disk size.
for f in stats/phone-G0LD.jsonl likes-phone-G0LD.jsonl pins-phone-G0LD.jsonl; do
    SIZE=$(stat -c '%s' "$META/$f")
    OFFSET=$(sqlite3 "$DB" "SELECT offset FROM ingest_offsets WHERE path='$f';")
    [ "$OFFSET" = "$SIZE" ] || { echo "FAIL: $f offset $OFFSET != size $SIZE"; exit 1; }
done

# 5. Idempotent re-run.
"$BIN" ingest --meta-dir "$META" 2>&1 | tee "$WORK/ingest2.log"
grep -qE 'plays=0 likes=0 pins=0 offsets_advanced=0' "$WORK/ingest2.log" \
    || { echo "FAIL: re-run not idempotent"; exit 1; }

# Counts unchanged after re-run.
PLAYS_AFTER=$(sqlite3 "$DB" "SELECT COUNT(*) FROM plays;")
[ "$PLAYS_AFTER" = "3" ] || { echo "FAIL: plays count changed after re-run"; exit 1; }

echo "test_jsonl_goldens: OK"
