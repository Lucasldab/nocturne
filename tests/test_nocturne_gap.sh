#!/usr/bin/env bash
# Unit test for config/bin/nocturne-gap: append + list, local mode.
#
# What it proves:
#   1. Two appends produce two valid JSONL lines with the frozen field order.
#   2. Newlines in the text are collapsed; surrounding space is trimmed.
#   3. -l merges desktop + phone files, oldest first, one line per gap.
#   4. Empty text and over-1000-char text are refused (exit 2), file untouched.
#   5. A trailing partial line (mid-Syncthing) does not break -l.
#
# Hermetic: NOCTURNE_GAP_HOST= runs everything locally against a temp meta dir.

set -euo pipefail

GAP="${GAP:-config/bin/nocturne-gap}"
test -f "$GAP" || { echo "missing: $GAP" >&2; exit 1; }

WORK=$(mktemp -d -t nocturne-gap-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
export NOCTURNE_GAP_HOST= NOCTURNE_GAP_META="$WORK"
fail=0

sh "$GAP" "want a sleep timer"
sh "$GAP" "  queue
can't reorder  "
F="$WORK/gaps-desktop.jsonl"

echo "==> case 1: two valid lines, frozen field order"
[ "$(wc -l < "$F")" -eq 2 ] || { echo "FAIL: expected 2 lines"; fail=1; }
keys=$(head -1 "$F" | jq -c 'keys_unsorted')
[ "$keys" = '["v","ts","text","screen","track","app"]' ] || { echo "FAIL: field order $keys"; fail=1; }
head -1 "$F" | jq -e '.v==1 and .screen=="desktop" and .track==null and .app=="desktop"' >/dev/null \
    || { echo "FAIL: fixed fields"; fail=1; }

echo "==> case 2: newlines collapsed, trimmed"
[ "$(sed -n 2p "$F" | jq -r .text)" = "queue can't reorder" ] || { echo "FAIL: got '$(sed -n 2p "$F" | jq -r .text)'"; fail=1; }

echo "==> case 3: -l merges files, oldest first"
printf '%s\n' '{"v":1,"ts":1577836800001,"text":"phone gap","screen":"now-playing","track":null,"app":"0.4.49-dev"}' \
    > "$WORK/gaps-phone-abcd1234.jsonl"
out=$(sh "$GAP" -l)
[ "$(printf '%s\n' "$out" | wc -l)" -eq 3 ] || { echo "FAIL: -l printed:"; echo "$out"; fail=1; }
printf '%s\n' "$out" | head -1 | grep -q 'now-playing  phone gap$' || { echo "FAIL: oldest not first"; echo "$out"; fail=1; }

echo "==> case 4: refusals"
before=$(wc -l < "$F")
if sh "$GAP" "   " 2>/dev/null; then echo "FAIL: blank accepted"; fail=1; fi
if sh "$GAP" "$(printf 'x%.0s' $(seq 1001))" 2>/dev/null; then echo "FAIL: 1001 chars accepted"; fail=1; fi
[ "$(wc -l < "$F")" -eq "$before" ] || { echo "FAIL: refused text was written"; fail=1; }

echo "==> case 5: trailing partial line tolerated"
printf '%s' '{"v":1,"ts":15778' >> "$WORK/gaps-phone-abcd1234.jsonl"
sh "$GAP" -l >/dev/null || { echo "FAIL: -l died on a partial line"; fail=1; }

[ "$fail" -eq 0 ] || { echo "==> FAILED"; exit 1; }
echo "==> PASS"
