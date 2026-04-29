# Syncthing-Fork phone setup — auto-pull configuration

After pinning a track in nocturne, the audio file should arrive on the phone
without you opening Syncthing-Fork. If you find yourself manually triggering
"Sync Now" before the file shows up as resident, one of the settings below is
wrong.

This is a one-time phone-side configuration. Repeat for both folders
(`nocturne-meta`, `nocturne-files`).

## 1. Per-folder settings — Syncthing-Fork

Open Syncthing-Fork → tap the folder → pencil/edit icon.

| Setting              | Value          | Why |
| -------------------- | -------------- | --- |
| Watch for changes    | **ON**         | Syncthing reacts to remote announces immediately instead of waiting for the next rescan. |
| Full rescan interval | **60 seconds** | Belt-and-suspenders — even if Watch misses an event, the folder is rechecked once a minute. |
| Folder type (`nocturne-files`) | **Receive Only** | Phone never edits audio files; protects against accidental local writes. |
| Folder type (`nocturne-meta`)  | **Send & Receive** | Phone writes stats/likes/pins; daemon reads them. |
| Ignore patterns      | leave default — `.stignore` is managed by the daemon | The daemon owns rotation; do not hand-edit. |

## 2. App-wide settings — Syncthing-Fork

Syncthing-Fork → Menu → Settings.

| Setting              | Value     | Why |
| -------------------- | --------- | --- |
| Run conditions → Always run in background | **ON** | Without this, syncing only happens while the app is foreground. |
| Run conditions → Run on mobile data | your call | If you'd rather not chew cellular for music, leave OFF and only sync on WiFi. |
| Battery → Run on battery | **ON** | Default is ON; verify it has not been flipped. |

## 3. GrapheneOS battery + background permissions

GrapheneOS aggressively restricts background apps. Without these
exceptions, the OS will kill Syncthing-Fork shortly after you leave it.

Settings → Apps → Syncthing-Fork:

| Setting                              | Value          |
| ------------------------------------ | -------------- |
| Battery → Background usage           | **Unrestricted** (NOT "Optimized") |
| Battery → Allow background activity  | **Allow**      |
| Notifications → Allow notifications  | **Allow** (Syncthing-Fork uses the persistent notification to keep itself alive — required) |
| Permissions → Files and media        | **Allow access to media only** is enough; full filesystem access not required |

If Syncthing-Fork still gets killed, also check Settings → Apps →
Syncthing-Fork → App battery usage and confirm it is NOT in any "Restricted"
or "Sleeping app" bucket.

## 3a. GrapheneOS battery + background — nocturne app

Same problem, same fix, different app. Without these exceptions GrapheneOS
will kill the nocturne `PlaybackService` mid-track when the screen turns off
(symptom: music stops mid-song, reopening the app shows an empty queue).
v0.4.14+ holds a `WAKE_MODE_LOCAL` partial wake-lock during playback, but
the OS can still terminate the foreground service if you leave background
activity restricted.

Settings → Apps → nocturne:

| Setting                              | Value           |
| ------------------------------------ | --------------- |
| Battery → Background usage           | **Unrestricted** |
| Battery → Allow background activity  | **Allow**       |
| Notifications → Allow notifications  | **Allow** (the media notification is what marks the service as foreground; required) |

The wake-lock is partial (CPU only, no screen wake) and is released
automatically when playback pauses or stops, so battery cost is the same
as any other music player.

## 4. Verifying

After the steps above:

1. Pin a track in nocturne (any not currently resident).
2. Within ~60 seconds, the file should land in the music folder. nocturne's
   Sync screen → `manifest` row → `$ refresh now` will flip it to resident
   (or wait up to 45s for the foreground poll loop).
3. If the file does not arrive in 60 seconds, open Syncthing-Fork and check
   the folder's "Last Scan" timestamp. Stale → Watch is off or background
   was killed.

## 5. What this does NOT cover

- Desktop-side Syncthing config — that is owned by the daemon (`nocturned`)
  and the user's main Linux Syncthing install. See
  `daemon/docs/syncthing-config.md` if it exists.
- Cellular-data warnings, low-battery throttling — those are intentional
  tradeoffs the user makes per Syncthing-Fork's "Run conditions" panel.
- Syncthing-Fork ↔ desktop Syncthing v2 protocol compatibility — both should
  be on Syncthing 2.0.16+ per the project's stack pins.
