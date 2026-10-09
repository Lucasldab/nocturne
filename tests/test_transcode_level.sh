#!/usr/bin/env bash
# `nocturned transcode-level` end to end, hermetic (own HOME, own DB).
#
# What it proves:
#   1. Without [transcode] level_lufs the command refuses (usage exit 64).
#   2. Dry-run plans the unlevelled resident and changes nothing.
#   3. --apply re-encodes it: the copy's loudness lands within 1 LU of the
#      target, the row records gain + source LUFS + the new size.
#   4. A row that already has a gain is never selected.
#   5. A row whose transcode_format differs from the config is skipped
#      (counted as format_mismatch), not re-encoded into the wrong container.
#   6. No temp file is left in resident/ (Syncthing-shared) or in
#      <library>/.transcode-level-tmp; a stale temp there is cleared.
#   7. A second --apply is a no-op (only the mismatch is planned again).
#
# Skips (exit 77) without ffmpeg/libopus or sqlite3.

set -euo pipefail

BIN="${BIN:-build/nocturned}"
test -x "$BIN" || { echo "missing: $BIN" >&2; exit 1; }
command -v sqlite3 >/dev/null || { echo "==> SKIP: sqlite3 CLI missing"; exit 77; }
# Not `grep -q`: it exits early, ffmpeg gets SIGPIPE, and pipefail reports that.
ffmpeg -hide_banner -encoders 2>/dev/null | grep libopus >/dev/null \
    || { echo "==> SKIP: ffmpeg with libopus missing"; exit 77; }

WORK=$(mktemp -d -t nocturne-level-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
export HOME="$WORK/home"
unset XDG_CONFIG_HOME XDG_DATA_HOME XDG_STATE_HOME XDG_CACHE_HOME
LIB="$WORK/library"
mkdir -p "$HOME/.config/nocturne" "$LIB/archive/A/B" "$LIB/resident/A/B"
CFG="$HOME/.config/nocturne/config.toml"
DB="$HOME/.local/share/nocturne/nocturne.db"

fail() { echo "FAIL: $*" >&2; exit 1; }
measure() {
    ffmpeg -nostdin -hide_banner -nostats -i "$1" -map 0:a \
        -af loudnorm=print_format=json -f null - 2>&1 \
        | sed -n 's/.*"input_i" : "\(.*\)",/\1/p'
}

# Loud source (~ -9 LUFS) and an unlevelled resident copy of it.
SRC="$LIB/archive/A/B/t.flac"
OUT="$LIB/resident/A/B/t.opus"
ffmpeg -nostdin -loglevel error -y -f lavfi -i 'sine=frequency=440:duration=8' \
    -af volume=4 -ac 2 "$SRC"
ffmpeg -nostdin -loglevel error -y -i "$SRC" -c:a libopus -b:a 128k "$OUT"
# A second resident that is already levelled: must be left alone.
SRC2="$LIB/archive/A/B/u.flac"; OUT2="$LIB/resident/A/B/u.opus"
cp "$SRC" "$SRC2"; cp "$OUT" "$OUT2"
before2=$(sha256sum "$OUT2" | cut -d' ' -f1)
# A third, written as AAC under an older config: must be skipped, not rewritten.
SRC3="$LIB/archive/A/B/v.flac"; OUT3="$LIB/resident/A/B/v.m4a"
cp "$SRC" "$SRC3"; printf 'm4a' > "$OUT3"
# A stale temp from an "interrupted" earlier run.
mkdir -p "$LIB/.transcode-level-tmp"; printf 'junk' > "$LIB/.transcode-level-tmp/zz.opus"

cat > "$CFG" <<EOF
[library]
path = "$LIB"

[transcode]
enabled = 1
format = "opus"
bitrate_kbps = 128
EOF

# 1. level_lufs unset -> refuse.
set +e; "$BIN" transcode-level >/dev/null 2>&1; rc=$?; set -e
[ "$rc" = 64 ] || fail "expected usage exit 64 without level_lufs, got $rc"
echo "level_lufs = -18" >> "$CFG"
# An empty dry run creates and migrates the DB.
"$BIN" transcode-level 2>/dev/null | grep "planned=0" >/dev/null || fail "empty dry run"
[ -f "$DB" ] || fail "DB not created at $DB"

now=2026-10-10T00:00:00Z
sqlite3 "$DB" <<EOF
INSERT INTO tracks (sha256, path, mtime_ns, size_bytes, format, date_added, last_seen_at)
VALUES ('aa', '$SRC', 0, 1, 'flac', '$now', '$now'),
       ('bb', '$SRC2', 0, 1, 'flac', '$now', '$now'),
       ('cc', '$SRC3', 0, 1, 'flac', '$now', '$now');
INSERT INTO residency_state (sha256, location, updated_at, transcode_path,
                             transcode_size_bytes, transcode_format, transcode_gain_db)
VALUES ('aa', 'resident', '$now', '$OUT', 1, 'opus', NULL),
       ('bb', 'resident', '$now', '$OUT2', 1, 'opus', -2.5),
       ('cc', 'resident', '$now', '$OUT3', 1, 'aac', NULL);
EOF
before=$(sha256sum "$OUT" | cut -d' ' -f1)

# 2. Dry run.
out=$("$BIN" transcode-level 2>/dev/null)
echo "$out" | grep -q "planned=2 levelled=0" || fail "dry-run: $out"
[ "$(sha256sum "$OUT" | cut -d' ' -f1)" = "$before" ] || fail "dry-run modified the copy"

# 3. Apply.
out=$("$BIN" transcode-level --apply 2>/dev/null)
echo "$out" | grep -q "planned=2 levelled=1 missing_source=0 format_mismatch=1 errors=0" \
    || fail "apply: $out"
[ "$(cat "$OUT3")" = "m4a" ] || fail "format-mismatch copy was rewritten"
lufs=$(measure "$OUT")
python3 -c "import sys; v=float('$lufs'); sys.exit(0 if abs(v+18)<=1 else 1)" \
    || fail "levelled copy measures $lufs LUFS, want -18 +-1"
row=$(sqlite3 "$DB" "SELECT transcode_gain_db < -3, abs(transcode_lufs + 9) < 1.5,
                            transcode_size_bytes = $(stat -c %s "$OUT")
                     FROM residency_state WHERE sha256='aa'")
[ "$row" = "1|1|1" ] || fail "row not updated as expected: $row"
ls -A "$LIB/resident/A/B" | grep -v -E '^[tuv]\.(opus|m4a)$' && fail "stray file in resident/"
[ -z "$(ls -A "$LIB/.transcode-level-tmp")" ] || fail "temp dir not empty (stale temp not cleared?)"

# 4. Already-levelled row untouched.
[ "$(sha256sum "$OUT2" | cut -d' ' -f1)" = "$before2" ] || fail "levelled row was re-encoded"
[ "$(sqlite3 "$DB" "SELECT transcode_gain_db FROM residency_state WHERE sha256='bb'")" = "-2.5" ] \
    || fail "levelled row's gain changed"

# 5. Idempotent.
out=$("$BIN" transcode-level --apply 2>/dev/null)
echo "$out" | grep -q "planned=1 levelled=0 missing_source=0 format_mismatch=1" \
    || fail "second apply not a no-op: $out"

echo "==> transcode-level: all checks passed"
