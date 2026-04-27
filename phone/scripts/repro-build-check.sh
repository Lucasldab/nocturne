#!/usr/bin/env bash
# CROSS-02 scaffold gate (informational this phase; Phase 8 tightens to
# byte-identical cross-machine verification).
#
# Build the APK twice on the SAME machine and report whether the unsigned
# APK contents (everything except META-INF/signing material) are
# byte-identical. Phase 4 expectation: same machine + clean build →
# identical (the determinism flags should make this true).
set -euo pipefail

cd "$(dirname "$0")/.."   # phone/

echo "repro-build: pass 1"
./gradlew clean :app:assembleDebug --quiet --no-configuration-cache
APK1=app/build/outputs/apk/debug/nocturne-phone-debug.apk
[[ -f "$APK1" ]] || { echo "repro-build: pass 1 produced no APK" >&2; exit 2; }
cp "$APK1" /tmp/nocturne-phone-pass1.apk

echo "repro-build: pass 2"
./gradlew clean :app:assembleDebug --quiet --no-configuration-cache
APK2=app/build/outputs/apk/debug/nocturne-phone-debug.apk
[[ -f "$APK2" ]] || { echo "repro-build: pass 2 produced no APK" >&2; exit 2; }
cp "$APK2" /tmp/nocturne-phone-pass2.apk

# Strip signing metadata (debug key changes per build); compare class+resource bytes.
strip_signing() {
  local in="$1" out="$2"
  local tmp
  tmp=$(mktemp -d)
  unzip -q "$in" -d "$tmp"
  rm -rf "$tmp/META-INF"
  ( cd "$tmp" && find . -type f -print0 | sort -z | xargs -0 sha256sum ) > "$out"
  rm -rf "$tmp"
}
strip_signing /tmp/nocturne-phone-pass1.apk /tmp/repro-pass1.txt
strip_signing /tmp/nocturne-phone-pass2.apk /tmp/repro-pass2.txt

if diff -q /tmp/repro-pass1.txt /tmp/repro-pass2.txt >/dev/null; then
  echo "repro-build: OK — passes are byte-identical (excluding signing)"
  exit 0
else
  echo "repro-build: DRIFT detected — diff:" >&2
  diff /tmp/repro-pass1.txt /tmp/repro-pass2.txt | head -40 >&2
  echo "repro-build: (Phase 4 gate is informational; Phase 8 will fail builds on drift)"
  exit 0
fi
