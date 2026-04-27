#!/usr/bin/env bash
# CROSS-01 audit: assert APK declares ONLY the allowlisted permissions.
#
# Phase 5 allowlist (extends Phase 4):
#   - android.permission.READ_MEDIA_AUDIO                  (declared)
#   - android.permission.POST_NOTIFICATIONS                (declared, runtime grant on first play)
#   - android.permission.FOREGROUND_SERVICE                (PlaybackService)
#   - android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK (mediaPlayback type)
#
# ExoPlayer's audio focus (handleAudioFocus=true) does not require any
# additional audio-settings permissions beyond the above.
set -euo pipefail

APK="${1:-phone/app/build/outputs/apk/debug/nocturne-phone-debug.apk}"
AAPT2="${AAPT2:-/home/lucas/Android/build-tools/35.0.0/aapt2}"

if [[ ! -f "$APK" ]]; then
  echo "audit-permissions: APK not found at $APK" >&2
  exit 2
fi
if [[ ! -x "$AAPT2" ]]; then
  echo "audit-permissions: aapt2 not found at $AAPT2 (override via env AAPT2=...)" >&2
  exit 2
fi

ALLOWED=(
  "android.permission.READ_MEDIA_AUDIO"
  "android.permission.POST_NOTIFICATIONS"
  # Phase 5: PlaybackService FGS (mediaPlayback). audit-permissions confirms
  # only these two new perms appear; Pitfall 4 — no BOOT_COMPLETED.
  "android.permission.FOREGROUND_SERVICE"
  "android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK"
  # AGP 8.x auto-injects a per-app DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION
  # so dynamically registered receivers default to non-exported. Synthetic,
  # not an outward permission.
  "io.nocturne.phone.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION"
)

# Pull all uses-permission names from aapt2's dump permissions output.
mapfile -t FOUND < <("$AAPT2" dump permissions "$APK" \
  | awk -F"'" '/^uses-permission: name=/ { print $2 }')

FAIL=0
for p in "${FOUND[@]}"; do
  [[ -z "$p" ]] && continue
  matched=0
  for a in "${ALLOWED[@]}"; do
    if [[ "$p" == "$a" ]]; then
      matched=1
      break
    fi
  done
  if [[ "$matched" -eq 0 ]]; then
    echo "audit-permissions: DISALLOWED permission: $p" >&2
    FAIL=1
  fi
done

if [[ "$FAIL" -eq 0 ]]; then
  echo "audit-permissions: OK"
  echo "Found permissions:"
  printf '  %s\n' "${FOUND[@]}"
fi
exit "$FAIL"
