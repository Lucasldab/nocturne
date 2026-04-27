# Phase 5 Hardware-Acceptance Checklist

Five playback acceptance procedures that require a physical GrapheneOS device and
(for PLAY-05) a Bluetooth headset. These cannot be emulated in Robolectric.

Run once the phone arrives and Syncthing has synced at least one album's audio
files to the device.

---

## PLAY-03 — 30-Minute Background Survival

**Requirement:** PlaybackService continues playing for at least 30 minutes with
the screen off and the app removed from the foreground.

### Setup

1. Install the debug APK: `adb install phone/app/build/outputs/apk/debug/nocturne-phone-debug.apk`
2. Grant POST_NOTIFICATIONS on first launch (tap Allow in the permission dialog).
3. Ensure at least one album is resident (isResident = true in the catalog DB).
4. Connect wired headphones or a Bluetooth headset.

### Procedure

1. Open the app, navigate to Albums, tap an album, tap the first track.
2. Verify the Now Playing screen appears and the track starts.
3. Press the power button to lock the screen.
4. Wait 30 minutes. Do not touch the phone.
5. After 30 minutes, unlock the screen.

### Expected Behaviour

- Audio continues throughout the 30-minute window without interruption.
- The lock-screen notification shows album art, track title, and playback controls.
- The notification is still visible when the screen is unlocked.
- `adb shell dumpsys media_session` lists nocturne's session as ACTIVE.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| Audio stops after ~1 min | Battery optimizer killed FGS | Settings → Apps → nocturne → Battery → Unrestricted |
| Audio stops after ~10 min | Doze mode disrupted Media3 wake lock | Same mitigation; also check if `WAKE_LOCK` is in APK permissions |
| No lock-screen notification | POST_NOTIFICATIONS not granted | Settings → Apps → nocturne → Notifications → Allow |

---

## PLAY-04 — Reboot Resumption

**Requirement:** After a device reboot, pressing the Bluetooth media button (or
the lock-screen "Play" button) resumes the previously-playing queue at the saved
track and position.

### Setup

1. Install the APK and sync at least one album.
2. Play an album to mid-track (e.g. 2:30 into a 5-minute track).
3. Pause (the queue state is persisted by `QueueRepository.saveQueue` on
   `onIsPlayingChanged` → debounce 500ms → DataStore write).

### Procedure

1. While paused, reboot the phone: `adb reboot` or Settings → System → Reboot.
2. Wait for the device to fully boot (unlock the screen).
3. Press the Bluetooth media-button Play action, OR open the notification shade
   and tap Play on the lock-screen media card.

### Expected Behaviour

- Media3 calls `PlaybackService.onPlaybackResumption` via `MediaButtonReceiver`.
- The service reads `QueueRepository.loadQueue()`, calls
  `PlaybackResumption.toMediaItemsWithStartPosition`, and returns the saved queue.
- Playback resumes at the saved track and position (within a few seconds tolerance
  for ExoPlayer's buffering start).
- The lock-screen card shows the correct album art + track title.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| Play button does nothing | MediaButtonReceiver not declared | `grep MediaButtonReceiver phone/app/src/main/AndroidManifest.xml` must return 1 |
| Resumes from track 1, position 0 | QueueRepository failed to persist | Check DataStore file: `adb shell run-as io.nocturne.phone ls files/datastore/` |
| Track list cleared (empty queue) | TrackDao.byId returned null for all IDs | Catalog may not have been imported — re-run SAF import from FirstRunScreen |

---

## PLAY-05 — Bluetooth Transport + Becoming-Noisy

**Requirement:** Audio pauses automatically when BT headphones disconnect or
wired headphones are unplugged. Audio resumes on reconnect only if the user
explicitly presses Play.

### Setup

1. Install the APK and sync at least one album.
2. Pair a Bluetooth headset with the phone.
3. Connect the headset and start playback.

### Procedure A — BT Disconnect (Becoming-Noisy)

1. With music playing, turn off the BT headset.
2. Observe playback.

### Procedure B — BT Reconnect (No Auto-Resume)

1. Turn the BT headset back on.
2. Observe playback.

### Procedure C — Wired Headphone Unplug

1. With wired headphones and music playing, unplug the headphones.
2. Observe playback.

### Expected Behaviour

- Procedure A: Audio pauses immediately. The lock-screen notification switches
  to paused state. No audio plays from the phone speaker.
- Procedure B: Audio does NOT auto-resume. The user must press Play.
- Procedure C: Same as Procedure A — audio pauses immediately.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| Audio continues through speaker on disconnect | `setHandleAudioBecomingNoisy(false)` or missing | Check PlaybackService ExoPlayer builder |
| Audio auto-resumes on reconnect | Incorrect audio-focus policy | ExoPlayer should not auto-resume on audio focus re-gain after becoming-noisy |

---

## PLAY-06 — Audio Focus on Incoming Call

**Requirement:** Music volume ducks or pauses during a phone call, and resumes
after the call ends (or stays paused if interrupted for more than ~30 seconds).

### Setup

1. Install the APK and sync at least one album.
2. Start music playback.
3. Call the device from another phone.

### Procedure

1. With music playing, make an incoming call from another device.
2. Answer the call.
3. End the call.

### Expected Behaviour

- When the phone rings: ExoPlayer receives `AUDIOFOCUS_LOSS_TRANSIENT`; audio
  pauses (or ducks to very low volume — ExoPlayer's default is to pause).
- During the call: no music audio.
- After the call ends: ExoPlayer receives `AUDIOFOCUS_GAIN`; music resumes
  from where it was paused.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| Music continues during call | `handleAudioFocus = false` in ExoPlayer builder | Check PlaybackService; must be `true` |
| Music does not resume after call | ExoPlayer paused permanently on LOSS | ExoPlayer 1.10 handles this by default; check if `setHandleAudioBecomingNoisy` interferes |

---

## PLAY-09 — Lock-Screen Album Art (Android 14 / 15 / 16)

**Requirement:** Album art appears on the lock-screen media notification across
Android 14, 15, and 16 (GrapheneOS). Art is scaled to 300x300 JPEG by
`ArtworkLoader.scaleTo300` before being set on `MediaMetadata`.

### Setup

1. Install the APK and sync at least one album whose tracks have embedded APIC
   (cover art) frames in their ID3v2 / Vorbis METADATA_BLOCK_PICTURE tags.
2. Lock the screen before starting playback.

### Procedure

1. Start music from the Albums screen (tap album → tap track).
2. Press the power button to lock the screen.
3. Observe the lock-screen media card.
4. Skip to the next track via the lock-screen control.

### Expected Behaviour

- The lock-screen media card shows:
  - Album art (300x300 JPEG, correctly aspect-ratio scaled, no pixelation)
  - Track title and artist name
  - Play/pause + skip controls
- When skipping to a different album, the art updates without flickering.
- On Android 16 (Pixel 9): the Dynamic Colour system may tint the lock screen
  based on the album art — this is expected behaviour from the OS, not a bug.

### Failure Modes

| Symptom | Likely Cause | Diagnostic |
|---------|-------------|------------|
| No art on lock screen | Artwork bytes > 512KB (Binder IPC limit) | `ArtworkLoader.scaleTo300` should keep output well below this; check ArtworkLoader output size |
| Art shows for first track only | `replaceMediaItem` guard tripping on equal bytes | Check `ArtworkLoader.scaleTo300` — if it returns the original bytes unchanged, the guard skips the update |
| Art not showing at all | No embedded art in the audio file | Verify with `kid3` or `ffprobe -show_streams file.flac \| grep -i cover` |

---

## Hardware-Blocked Status (Phase 5)

| Requirement | Status | Notes |
|-------------|--------|-------|
| PLAY-03 | Hardware-blocked | Needs physical phone |
| PLAY-04 | Hardware-blocked | Needs physical phone + reboot |
| PLAY-05 | Hardware-blocked | Needs physical phone + BT headset |
| PLAY-06 | Hardware-blocked | Needs physical phone + second phone for call |
| PLAY-09 | Hardware-blocked | Needs physical phone + tracks with embedded art |
| PLAY-10 | Partially hardware-blocked | Local persistence verified by PinDaoTest; daemon-side JSONL emission is Phase 6 |

See also: `docs/phone-setup.md` §6 (SYNC-07 Receive-Only tampering) and §7 (SYNC-06
5-minute cross-device timing) — both blocked on the same hardware.
