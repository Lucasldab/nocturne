# Phase 6 Hardware-Acceptance Checklist

Five stats-writer acceptance procedures that require a physical GrapheneOS
device. The 72-hour offline simulation (STATS-05) is the highest-risk test
in the project and the gate for closing Phase 6's hardware-blocked surface.

Prerequisites:
- Debug APK installed: `adb install phone/app/build/outputs/apk/debug/nocturne-phone-debug.apk`
- Syncthing-Fork on the phone configured with the `sync-meta` folder pair
  from the desktop (per `docs/phone-setup.md`).
- Phone has completed first-run SAF folder picker (metaTreeUri persisted in
  DataStore from Phase 4).
- At least one album resident on the phone (`adb shell ls /sdcard/Music/...`
  shows files), so plays / skips can be observed.
- Desktop has the `nocturned` binary built (`make` in repo root) for the
  ingest verification step.

---

## STATS-01 — Play Event Above Threshold

**Requirement:** Playing a track past `min(durationMs / 2, 240_000ms)` emits
a `kind=play` line in `stats/phone-<deviceid>.jsonl` within 1 second of the
track ending or the user pressing Next.

### Setup

1. Identify a resident track >= 4 minutes long (so the threshold is a flat
   240_000ms — the 50% rule is dominated by the 4-min cap).
2. Note the phone's deviceId for grep filtering after:
   ```
   adb shell run-as io.nocturne.phone cat files/datastore/nocturne_sync.preferences_pb \
     | strings | grep -A1 device_id
   ```
   (You may also read it from the next-emitted JSONL filename.)

### Procedure

1. Open the app, navigate to Albums, tap the long track, let it play to natural completion.
2. Wait for the next track to begin (auto-advance, DISCONTINUITY_REASON_AUTO_TRANSITION).
3. Pull the JSONL file from the phone:
   ```
   adb shell ls -la /sdcard/Documents/sync-meta/stats/
   adb pull /sdcard/Documents/sync-meta/stats/phone-<deviceid>.jsonl /tmp/
   ```

### Expected Behaviour

- The file ends with a new line of shape `{"v":1,"ts":<recent>,"kind":"play","track":"<sha>","played_ms":<>=240000>,"duration_ms":<full>}`.
- The line ends with exactly one LF (0x0A) and no CRLF.
- Byte-diff against the golden line shape (substituting ts / track / played_ms / duration_ms):
  ```
  head -1 tests/fixtures/jsonl-goldens/stats-golden.jsonl
  tail -1 /tmp/phone-<deviceid>.jsonl
  ```
  Field order, key spelling, and value types match.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| No new line in file | Listener not firing on AUTO_TRANSITION | `adb logcat -s 'StatsListener' '*:E'` for crash; confirm `PlaybackService.onCreate` adds StatsListener |
| Line emits with `duration_ms: 0` | Duration not cached at STATE_READY (Pitfall 1) | Restart playback; if still 0, Media3 1.10 changed contentDuration semantics |
| Line emits with CRLF | `System.lineSeparator()` regression | `grep -c 'System.lineSeparator' phone/app/src/main/kotlin/io/nocturne/phone/data/stats/JsonlFileWriter.kt` must be 0 |

---

## STATS-02 — Skip Event Under Threshold

**Requirement:** Plays under `min(durationMs/2, 240_000ms)` emit `kind=skip`.
Both AUTO_TRANSITION and user-tapped Next that crosses a media-item boundary
qualify.

### Setup

1. Resident album with at least three tracks.

### Procedure A — User-tapped skip mid-song

1. Tap the first track. Let 5–10 seconds pass.
2. Tap the Next button (or the lock-screen / BT next-track control).
3. Pull the JSONL file.

### Procedure B — Auto-advance skip (very short track)

1. Play a track shorter than 8 seconds (synthesise via ffmpeg if needed) to completion.
2. Pull the JSONL file.

### Expected Behaviour

- Procedure A: a new line of shape `{"v":1,"ts":<recent>,"kind":"skip",...}` with `played_ms` ~ the listened duration (5–10 s).
- Procedure B: a new line with `kind=skip` because `duration_ms < 16_000` and 50% of that is < the 50%-of-8s = 4_000 — but if you played to natural end, `played_ms ~ duration_ms`, so this should actually emit `kind=play`. Use a track of ~5s where you tap Next at 1s for a clearer skip.
- Both file end with LF; byte shape matches likes-golden's mirror except `kind=skip`.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| User-tap-Next fires no event | `decideEmission` rejects DISCONTINUITY_REASON_SEEK | Confirm `oldPosition.mediaItemIndex != newPosition.mediaItemIndex` triggers emission per D-13 |
| Tap-Next emits `kind=play` | Threshold misconfigured | Check `MAX_THRESHOLD_MS = 240_000L` in PlayClassifier.kt |

---

## STATS-03 — Like / Unlike / Pin / Unpin Tombstones

**Requirement:** Each like / unlike / pin / unpin action emits exactly one
JSONL line with the correct `(unit, id, liked|pinned)` shape and a fresh `ts`.

### Setup

1. Resident track playing on the NowPlayingScreen.
2. A separate non-resident track visible in TracksScreen with a PinChip.

### Procedure A — Like / Unlike

1. On NowPlayingScreen, tap the heart (outline -> filled, primary tint).
2. Tap again (filled -> outline, onSurface tint).
3. Pull `likes-phone-<deviceid>.jsonl`.

### Procedure B — Pin / Unpin

1. In TracksScreen, find a non-resident track. Tap its PinChip (state -> primary border).
2. Tap again (state -> onSurfaceVariant border).
3. Pull `pins-phone-<deviceid>.jsonl`.

### Expected Behaviour

- Procedure A: file contains two lines:
  `{"v":1,"ts":<t1>,"unit":"track","id":"<sha>","liked":true}`
  `{"v":1,"ts":<t2>,"unit":"track","id":"<sha>","liked":false}` with t2 > t1.
- Procedure B: file contains two lines:
  `{"v":1,"ts":<t1>,"unit":"track","id":"<sha>","pinned":true}`
  `{"v":1,"ts":<t2>,"unit":"track","id":"<sha>","pinned":false}` with t2 > t1.
- Album-level rows (unit="album") not exercised in Phase 6 UI (deferred to v1.x).
- Each file ends with LF; field order matches goldens.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| Heart tap does not emit JSONL | LikesWriter.record blocked on metaTreeUri null | Confirm Phase 4 first-run SAF picker completed |
| Second pin tap deletes Room row instead of flipping pinned | togglePin used dao.delete | Confirm togglePin uses `setPinned(id, !existing.pinned, ts)` per 06-04 |

---

## STATS-05 — 72-Hour Offline Simulation (HIGHEST RISK)

**Requirement:** After the phone has been offline for at least 72 hours of
normal listening, on reconnect to home WiFi all stats events arrive at the
desktop within 1 minute. This is the multi-day-offline-tolerance test —
the hardest gate in the project.

### Setup

1. Phone has at least 12 GB of resident music (~1 album per planned listen
   session; pin from the catalog if necessary).
2. Desktop is at a known-clean ingest state:
   ```
   cd /home/projects/nocturne
   sqlite3 ~/.local/state/nocturne/nocturned.db 'SELECT path, byte_offset FROM ingest_offsets;'
   ```
3. Disable WiFi on the phone (`adb shell settings put global wifi_on 0`).
4. Verify Syncthing-Fork on the phone is running but in offline state
   (only LAN peer should appear unreachable).

### Procedure

1. Day 0: Disable WiFi. Play music throughout the day in your normal pattern
   (mix of plays-to-end, mid-track skips, several pins / likes).
2. Day 1, Day 2, Day 3: Continue normal listening. Random pin/unpin a few
   albums. Like a few tracks. Aim for >= 50 stats events accumulated over
   the 72h window.
3. Verify before reconnect that the JSONL files have grown:
   ```
   adb shell wc -l /sdcard/Documents/sync-meta/stats/phone-*.jsonl
   adb shell wc -l /sdcard/Documents/sync-meta/likes-phone-*.jsonl
   adb shell wc -l /sdcard/Documents/sync-meta/pins-phone-*.jsonl
   ```
   Expected: line counts equal to the events you logged across 72h.
4. End of day 3: Re-enable WiFi (`adb shell settings put global wifi_on 1`).
5. Start a stopwatch. Wait for Syncthing-Fork to mark the meta folder
   "Up to Date" (visible in Syncthing-Fork's Folders view).
6. On the desktop, run the ingester:
   ```
   cd /home/projects/nocturne
   ./build/nocturned ingest --meta-dir ~/.local/share/syncthing/sync-meta
   ```

### Expected Behaviour

- Step 5: Syncthing reports "Up to Date" within 1 minute of WiFi rejoin
  (assumes the meta folder is well under 1 MB total — JSONL lines are tiny).
- Step 6: `nocturned ingest` reports a non-zero count of new events ingested
  across the three streams (plays, likes, pins).
- Verify desktop SQLite reflects the new data:
  ```
  sqlite3 ~/.local/state/nocturne/nocturned.db 'SELECT COUNT(*) FROM plays;'
  sqlite3 ~/.local/state/nocturne/nocturned.db 'SELECT COUNT(*) FROM likes WHERE unit="track";'
  sqlite3 ~/.local/state/nocturne/nocturned.db 'SELECT COUNT(*) FROM pins;'
  ```
- The combined wall-clock from "WiFi back on" to "ingest --dry-run reports zero new events on second run" is <= 60 seconds.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| JSONL files have far fewer lines than events logged | Doze killed PlaybackService while pinned/liked | Re-check FGS exemption: Settings -> Apps -> nocturne -> Battery -> Unrestricted |
| Syncthing-Fork shows "Out of Sync" indefinitely | Meta folder selective-sync misconfigured | Reread `docs/phone-setup.md` §3 — meta folder must be Send-Receive both ends |
| `nocturned ingest` reports `parse_error > 0` | Byte-shape regression on the phone | Diff the failing line against the golden; confirm ByteShapeGoldenTest is still passing |
| Sync wall-clock > 1 min | WiFi pairing slow / phone backed off | Re-verify on a second 72h cycle; the test is the multi-day flush, not WiFi reconnect speed |

---

## STATS-06 — Last-Sync Surface in Settings

**Requirement:** The phone surfaces a relative-time string indicating when
the phone last successfully appended an event to a JSONL file. This is a
PROXY for "ready to sync" — the spec acknowledges (D-27) that desktop
ingestion completion is not visible to the phone.

### Setup

1. Phone with no events ever logged (fresh install or fresh DataStore wipe).

### Procedure A — Empty state

1. Tap the Settings tab in the bottom NavigationBar.
2. Read the row under `STATS SYNC`.

### Procedure B — After events

1. Play a track to completion (STATS-01 emits a play event).
2. Return to the Settings tab.
3. Read the row again.
4. Wait 5 minutes; return to Settings; read again.

### Expected Behaviour

- Procedure A: row reads `No events logged yet` (verbatim).
- Procedure B step 2-3: row reads `Last event logged: just now`.
- Procedure B step 4: row reads `Last event logged: 5 minutes ago`.
- Caption row always reads `Sync delivery depends on WiFi and Syncthing.`

**Verbatim copy contract** (UI-SPEC; any drift fails STATS-06):
- Empty state row: "No events logged yet"
- Just-after-event row: "Last event logged: just now"
- Caption (always present): "Sync delivery depends on WiFi and Syncthing."

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| Row reads "1970-01-01" or similar | lastStatsSyncAt set to 0 instead of nowMs() | Confirm JsonlFileWriter calls `setLastStatsSyncAt(nowMs())` AFTER `pfd.sync()` |
| Row never updates | Settings does not recompose on Flow emission | Confirm SettingsScreen uses collectAsStateWithLifecycle |

---

## Hardware-Blocked Status (Phase 6)

| Requirement | Status | Notes |
|-------------|--------|-------|
| STATS-01 | Hardware-blocked | Code complete; needs physical phone for Media3 listener event timing |
| STATS-02 | Hardware-blocked | Code complete; needs physical phone for tap-Next gesture |
| STATS-03 | Code complete | Local persistence + JSONL emission verified by unit tests; on-device confirmation per Procedure A/B above |
| STATS-04 | Code complete | File naming + fsync verified by unit tests; durability under Doze is the hardware concern |
| STATS-05 | Hardware-blocked | The 72h offline simulation IS the hardware test; cannot be approximated |
| STATS-06 | Code complete | RelativeTimeFormatterTest covers all buckets; on-device confirmation per Procedure A/B above |

See also: `phone/docs/phase-5-hardware-acceptance.md` for the player-side
procedures these stats events are observed alongside.
