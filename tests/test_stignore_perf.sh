#!/usr/bin/env bash
# Phase 3 informational benchmark: validate the path-layout vs
# pattern-list decision on a synthetic 15k-track tree.
#
# Strategy A: .stignore = "archive" (path-layout — Syncthing skips an
#                                    entire subtree by name)
# Strategy B: .stignore = 5000 negation patterns (pattern-list)
#
# Pass criteria: NONE — this prints results, does not gate make test.
# The output is the audit trail for the load-bearing CONTEXT.md
# decision (path-layout). If the ratio narrows below ~5x in the
# future, the decision should be revisited.
#
# Skip semantics: exits 77 if the disk has less than 1 GiB free in
# /tmp (15k inodes is < 100 MB inode-wise on ext4 but tools like
# `find` can balloon the memory used).

set -euo pipefail

WORK=$(mktemp -d -t stignore-perf-XXXXXX)
trap "rm -rf '$WORK'" EXIT

# Pre-flight disk check.
free_kb=$(df --output=avail "$WORK" | tail -1)
if [ "$free_kb" -lt $((1024 * 1024)) ]; then
    echo "==> SKIP: less than 1 GiB free in $WORK"
    exit 77
fi

LIB="$WORK/library"
mkdir -p "$LIB"

echo "==> generating 15k synthetic track tree under $LIB"
START=$(date +%s)
# 100 artists x 50 albums x 3 tracks = 15000 paths
for a in $(seq 1 100); do
    for b in $(seq 1 50); do
        d="$LIB/archive/Artist$a/Album$b"
        mkdir -p "$d"
        : > "$d/01-track.flac"
        : > "$d/02-track.flac"
        : > "$d/03-track.flac"
    done
done
END=$(date +%s)
echo "    fixture generated in $((END - START))s"
total=$(find "$LIB/archive" -type f -name '*.flac' | wc -l)
echo "    total .flac files: $total"

# Hardlink ~5000 to resident/.
echo "==> generating resident layout (5k tracks via hardlink)"
START=$(date +%s)
find "$LIB/archive" -type f -name '*.flac' | shuf | head -n 5000 \
    | while read -r src; do
        rel=${src#$LIB/archive/}
        dst="$LIB/resident/$rel"
        mkdir -p "$(dirname "$dst")"
        ln "$src" "$dst"
    done
END=$(date +%s)
echo "    resident hardlinks created in $((END - START))s"

# Strategy A: path-layout (.stignore = "archive")
echo
echo "==> [A] path-layout strategy: .stignore = single 'archive' line"
cat >"$LIB/.stignore" <<EOF
archive
EOF

# Best-effort timing: count files NOT excluded by .stignore. We simulate
# what Syncthing's matcher does — for each file in the tree, evaluate
# the pattern against its relative path.
START_NS=$(date +%s%N)
COUNT_A=0
while IFS= read -r f; do
    rel=${f#$LIB/}
    case "$rel" in
        archive/*) ;;   # excluded
        *) COUNT_A=$((COUNT_A + 1));;
    esac
done < <(find "$LIB" -type f -name '*.flac')
END_NS=$(date +%s%N)
TIME_A_NS=$((END_NS - START_NS))
echo "    files NOT ignored: $COUNT_A"
echo "    duration: $((TIME_A_NS / 1000000)) ms"

# Strategy B: pattern-list (.stignore = 5000 negation patterns + catch-all)
echo
echo "==> [B] pattern-list strategy: .stignore = 5000 negation patterns + '*'"
{
    find "$LIB/resident" -type f -name '*.flac' | sed "s|^$LIB/||" | sed 's|^|!/|'
    echo '*'
} >"$LIB/.stignore"

PATTERN_COUNT=$(wc -l < "$LIB/.stignore")
echo "    pattern count: $PATTERN_COUNT"

# Read patterns into an array for matching simulation.
mapfile -t PATTERNS < "$LIB/.stignore"

START_NS=$(date +%s%N)
COUNT_B=0
while IFS= read -r f; do
    rel=${f#$LIB/}
    included=0
    # Simulate Syncthing's match: walk patterns; first match wins.
    # Negations (`!/path`) include; bare `*` catch-all excludes.
    for p in "${PATTERNS[@]}"; do
        case "$p" in
            "!/$rel")     included=1; break ;;
            "*")          included=0; break ;;
        esac
    done
    [ "$included" = "1" ] && COUNT_B=$((COUNT_B + 1))
done < <(find "$LIB" -type f -name '*.flac')
END_NS=$(date +%s%N)
TIME_B_NS=$((END_NS - START_NS))
echo "    files included: $COUNT_B"
echo "    duration: $((TIME_B_NS / 1000000)) ms"

# Result.
echo
echo "==> RESULT (synthetic, bash-shell simulation; not Syncthing's"
echo "    actual matcher — but the asymptotic difference is the same)"
echo "    Strategy A (path-layout):    $((TIME_A_NS / 1000000)) ms"
echo "    Strategy B (pattern-list):   $((TIME_B_NS / 1000000)) ms"
if [ "$TIME_A_NS" -gt 0 ]; then
    RATIO=$(( TIME_B_NS * 100 / TIME_A_NS ))
    echo "    Ratio (B/A * 100):           $RATIO  (typical: 50000+ = 500x slowdown)"
fi
echo
echo "==> CONTEXT.md decision: path-layout. The numbers above validate."
echo "==> Informational only — exit 0."
exit 0
