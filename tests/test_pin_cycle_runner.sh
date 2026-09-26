#!/usr/bin/env bash
# Unit test for config/bin/nocturne-pin-cycle-runner's library resolution.
#
# What it proves:
#   1. The runner does NOT invent its own library path. nocturned resolves
#      the library from ~/.config/nocturne/config.toml; any path the runner
#      passes as argv[1] OVERRIDES that resolution.
#   2. With NOCTURNE_LIBRARY unset (the normal case — the systemd unit sets
#      no Environment=), the runner must hand nocturne-cycle-run nothing,
#      exactly like nocturne-cycle.service does on the timer path.
#   3. With NOCTURNE_LIBRARY set, that value is forwarded verbatim.
#   4. The freshness gate keys on ARRIVAL (ctime), not mtime: Syncthing
#      copies the phone's mtime, so a pin synced hours late carries an
#      mtime older than the manifest and must still open the gate.
#   5. actions-phone-*.jsonl (delete/unsync) opens the gate too.
#   6. Nothing arrived since the manifest -> gate stays shut.
#
# Regression guarded: the runner defaulted to "$HOME/music", a path that
# does not exist on hearth (library is /srv/bulk/music). Every pin from
# the phone aborted at `cycle: [1/5] scan` with rc=3 — 60 failures in 24h,
# silently, because the unit is triggered by a .path and nobody reads
# `systemctl --user status` for a oneshot.
#
# Hermetic: stubs nocturne-cycle-run, never runs a real cycle.

set -euo pipefail

RUNNER="${RUNNER:-config/bin/nocturne-pin-cycle-runner}"
test -x "$RUNNER" || test -f "$RUNNER" || { echo "missing: $RUNNER" >&2; exit 1; }

WORK=$(mktemp -d -t nocturne-pinrun-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

META="$WORK/meta"
mkdir -p "$META"
ARGS_FILE="$WORK/stub-args"

# Stub standing in for nocturne-cycle-run: record argv, exit clean.
STUB="$WORK/cycle-run-stub"
cat > "$STUB" <<STUBEOF
#!/usr/bin/env bash
printf '%s\n' "\$#" > "$ARGS_FILE"
printf '%s\n' "\$@" >> "$ARGS_FILE"
exit 0
STUBEOF
chmod +x "$STUB"

# reset_meta FILE... — manifest published first, then FILEs land after it.
# The gate compares ctimes; the short sleep guarantees the ordering even on
# filesystems with coarse timestamps.
reset_meta() {
    rm -rf "$META"; mkdir -p "$META"
    : > "$META/manifest.json"
    sleep 0.05
    local f
    for f in "$@"; do echo '{}' > "$META/$f"; done
}

reset_meta pins-phone-test.jsonl

fail=0

run_case() {
    local desc="$1"; shift
    rm -f "$ARGS_FILE"
    # HOME is sandboxed so that a runner which ignores NOCTURNE_CYCLE_RUN
    # (i.e. the pre-fix version) execs a nonexistent path and fails loudly
    # instead of kicking off a real cycle on the developer's machine.
    if ! env HOME="$WORK/home" \
             NOCTURNE_META_DIR="$META" \
             NOCTURNE_MANIFEST="$META/manifest.json" \
             NOCTURNE_CYCLE_RUN="$STUB" \
             "$@" bash "$RUNNER" >"$WORK/out" 2>&1; then
        echo "FAIL [$desc]: runner exited non-zero"; cat "$WORK/out"; fail=1; return
    fi
    if [ ! -f "$ARGS_FILE" ]; then
        echo "FAIL [$desc]: gate did not open — stub never ran"; cat "$WORK/out"; fail=1; return
    fi
}

echo "==> case 1: NOCTURNE_LIBRARY unset -> must pass no library argument"
run_case "unset" env -u NOCTURNE_LIBRARY
if [ -f "$ARGS_FILE" ]; then
    argc=$(head -1 "$ARGS_FILE")
    argv=$(tail -n +2 "$ARGS_FILE")
    if [ "$argc" -ne 0 ]; then
        echo "FAIL: expected argc=0 so nocturned reads config.toml; got argc=$argc argv='$argv'"
        fail=1
    else
        echo "    ok: argc=0 — library resolution left to config.toml"
    fi
fi

echo "==> case 2: NOCTURNE_LIBRARY set -> must forward it verbatim"
run_case "set" env NOCTURNE_LIBRARY=/srv/bulk/music
if [ -f "$ARGS_FILE" ]; then
    argc=$(head -1 "$ARGS_FILE")
    argv=$(tail -n +2 "$ARGS_FILE")
    if [ "$argc" -ne 1 ] || [ "$argv" != "/srv/bulk/music" ]; then
        echo "FAIL: expected argc=1 argv=/srv/bulk/music; got argc=$argc argv='$argv'"
        fail=1
    else
        echo "    ok: forwarded /srv/bulk/music"
    fi
fi

echo "==> case 3: pin synced late (phone mtime older than manifest) -> gate opens"
# Syncthing writes the file now but stamps the phone's old mtime on it.
reset_meta pins-phone-test.jsonl
touch -d '2020-01-01 00:00:00' "$META/pins-phone-test.jsonl"
run_case "late-sync" env -u NOCTURNE_LIBRARY
[ -f "$ARGS_FILE" ] && echo "    ok: late-arriving pin ran the cycle"

echo "==> case 4: only actions-phone (delete/unsync) arrived -> gate opens"
reset_meta actions-phone-test.jsonl
run_case "actions" env -u NOCTURNE_LIBRARY
[ -f "$ARGS_FILE" ] && echo "    ok: actions JSONL ran the cycle"

echo "==> case 5: nothing arrived after the manifest -> gate stays shut"
rm -rf "$META"; mkdir -p "$META"
echo '{}' > "$META/pins-phone-test.jsonl"
sleep 0.05
: > "$META/manifest.json"
rm -f "$ARGS_FILE"
env HOME="$WORK/home" NOCTURNE_META_DIR="$META" \
    NOCTURNE_MANIFEST="$META/manifest.json" NOCTURNE_CYCLE_RUN="$STUB" \
    bash "$RUNNER" >"$WORK/out" 2>&1 || { echo "FAIL [shut]: non-zero exit"; fail=1; }
if [ -f "$ARGS_FILE" ]; then
    echo "FAIL [shut]: cycle ran with nothing new"; fail=1
else
    echo "    ok: skipped"
fi

if [ "$fail" -ne 0 ]; then
    echo "==> FAILED"
    exit 1
fi
echo "==> PASS"
