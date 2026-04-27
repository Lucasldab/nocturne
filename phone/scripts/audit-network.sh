#!/usr/bin/env bash
# CROSS-01 deeper audit: assert classes.dex contains no java.net /
# OkHttp / Retrofit / Google / Firebase references.
#
# Catches dependency drift that AGP's permission merge wouldn't (a library
# that does NOT declare android.permission.INTERNET could still ship a
# network stack that we'd want to know about).
# Phase 6 (CONTEXT.md D-34, CROSS-01): JsonlFileWriter, StatsWriter,
# LikesWriter, PinsWriter, SettingsScreen — all write only to SAF tree URIs
# (no network surface). The BANNED list below already catches any drift.
# No update required to BANNED for Phase 6.
set -euo pipefail

APK="${1:-phone/app/build/outputs/apk/debug/nocturne-phone-debug.apk}"
if [[ ! -f "$APK" ]]; then
  echo "audit-network: APK not found at $APK" >&2
  exit 2
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

unzip -q "$APK" -d "$TMP" 'classes*.dex'

BANNED=(
  "Ljava/net/URL;"
  "Ljava/net/Socket;"
  "Ljava/net/HttpURLConnection;"
  "Lokhttp3/"
  "Lretrofit2/"
  "Lcom/google/firebase/"
  "Lcom/google/android/gms/"
  "Lokio/"
)

FAIL=0
for dex in "$TMP"/classes*.dex; do
  for sym in "${BANNED[@]}"; do
    if strings "$dex" | grep -qF "$sym"; then
      echo "audit-network: BANNED symbol '$sym' found in $(basename "$dex")" >&2
      FAIL=1
    fi
  done
done

if [[ "$FAIL" -eq 0 ]]; then
  echo "audit-network: OK — no banned network symbols in dex"
fi
exit "$FAIL"
