# Loudness levelling and download feedback — design

Date: 2026-10-10. Two independent features, one spec because both are small
and were requested together. Decisions marked **(user)** were made by the
owner; the rest are defaults chosen while the owner was away, flagged for
review.

## 1. Loudness levelling of resident transcodes

### Problem

The library is assembled one file at a time from Soulseek uploaders, so
mastering loudness varies wildly between neighbouring tracks on shuffle.
The phone has no volume levelling.

### Constraint that shapes the design

The phone never plays the FLACs. Every resident is a 128k Opus copy written
by `nocturned` (`transcode.c`, called from rotate's promote step). The usual
approach — beets `replaygain` writing tags into the FLACs — is ruled out:
nocturne's track id is the sha256 of the **whole FLAC file**, so rewriting
tags across the library would detach every pin, like and play.

So levelling happens **in the transcode**, which is lossy anyway: measure the
source, apply the gain while encoding. FLACs are never modified; the phone
app needs no change.

### Decisions

- **Per-track gain (user).** The library is overwhelmingly singletons and is
  listened to on shuffle; album gain would only matter for the few full
  albums.
- **Target -18 LUFS, ReplayGain 2 reference (user).** Nearly every modern
  master is louder (−8…−10 LUFS), so the common case is attenuation, which
  cannot clip.
- **No limiter.** Boost is capped so the true peak stays at or below
  −1 dBTP: `gain = min(target − I, −1 − TP)`. A quiet track with sharp peaks
  is boosted less than full rather than compressed.
- **Bounds.** Gain is clamped to [−30, +12] dB. A source that measures as
  silence or fails to measure (`I` ≤ −70 LUFS or unparseable) gets 0 dB and
  is logged — a measurement failure never fails the transcode.

### Mechanism

1. **Measure:** `ffmpeg -nostdin -i SRC -map 0:a -af loudnorm=print_format=json -f null -`
   (single analysis pass). Parse `input_i` and `input_tp` from the JSON block
   ffmpeg prints on stderr.
2. **Encode:** the existing encode command plus `-af volume=<gain>dB`.
3. **Record:** new columns on `residency_state` (migration `0009`):
   `transcode_gain_db REAL`, `transcode_lufs REAL`. `NULL` gain means "not
   levelled" (pre-feature copy, or levelling disabled).

`transcode.c` gains a pure, unit-tested core:
- `loudness_parse(const char *ffmpeg_stderr, double *i, double *tp)`
- `loudness_gain(double i, double tp, double target)` → dB

and `transcode_audio()` takes the levelling result as an out-parameter so
rotate can store it.

### Config

```toml
[transcode]
level_lufs = -18   # integer; 0 or absent = levelling off
```

Integer because the config parser only accepts integers for numbers.

### Existing residents

New command, same shape as `transcode-migrate`:

```
nocturned transcode-level [--apply]     # default dry-run
```

For every resident row with a transcode and `transcode_gain_db IS NULL`:
re-encode from the archive FLAC to a temp file beside the target, rename
over it atomically, update `transcode_size_bytes`, gain and LUFS. Holds the
daemon lock like the other mutating commands. Idempotent.

**Deployment cost (needs owner approval):** ~221 Opus files (~375 MB) are
rewritten on hearth and re-synced to the phone over Syncthing. Catalog
`size_bytes` changes for each.

### Testing

- Unit: `loudness_gain` (attenuate, peak-capped boost, clamps, silence);
  `loudness_parse` on captured ffmpeg output (normal, missing block, `-inf`).
- Integration (hearth, ffmpeg present): generate a loud sine fixture, run
  the levelled transcode, measure the output: within ±1 LU of −18.
- Migration test for `0009`.

## 2. Download feedback on the phone

### Problem

A phone download request that fails shows `flacget rc=1`. The owner cannot
tell a blocked artist from an MP3-only release from offline peers, so cannot
decide whether retrying with other words would help. A successful request
says nothing about *what* was fetched.

### Existing plumbing

`downloads-desktop.jsonl` lines already carry an optional free-text `msg`,
the phone stores it in `DownloadEntity.message`, and `SyncScreen` renders it
— for `error` rows only.

### Design

**Contract between `nocturned` and `flacget`:** `nocturned` sets
`FLACGET_RESULT_FILE` to a fresh temp file (in `$TMPDIR`, never in the
Syncthing-shared meta dir) before exec. `flacget` writes exactly one JSON
object to it before exiting:

```json
{"ok": false, "reason": "no_flac", "msg": "12 users have it, none as FLAC (lossy only)"}
```

`nocturned` reads it after `waitpid`, uses `msg` as the status line's `msg`
for both `done` and `error`, and falls back to today's messages if the file
is missing, empty or invalid. The exit code stays the verdict; the file is
only the explanation. Callers that don't set the variable see no change.

**Reasons** (one per exit path in flacget):

| reason | when | example msg |
|---|---|---|
| `fetched` | rc 0 | `Numb — Meteora (13) · CD 16/44.1 · 3:07` (+ `· searched without "Linkin Park" (blocked by Soulseek)` when the fallback fired) |
| `not_filed` | rc 3 | `downloaded, but beets would not file it — parked in staging/skipped` |
| `no_results` | 0 users even after the blocked-name fallback | `nothing on Soulseek for "…", even without the artist name` |
| `no_flac` | users answered, no acceptable FLAC | `N users have it, none as FLAC (lossy only)` / `FLACs found, none matching the title` |
| `peers_failed` | every candidate's transfer failed | `4 uploaders tried; all went offline or refused` |
| `gate_rejected` | too many lossy-sourced / wrong-recording rejects | `downloads rejected as transcoded from lossy — may only exist as MP3` |
| `spotify_unresolved` | link could not be read | `could not read that Spotify link` |
| `error` | search/API/pull/import failure | the underlying error, truncated |

**Phone:** `SyncScreen.DownloadRow` also shows `msg` for `done` rows, in the
muted colour. No schema change.

### Testing

- `flacget`: run with a temp result file against a stubbed path where
  practical; at minimum `py_compile` plus a dry-run against live slskd on
  hearth for `fetched`, `no_results` and `no_flac`.
- `nocturned`: a C test that runs `download` against a fake `flacget`
  script writing a known result file, asserting the status line's `msg` for
  `done`, `error`, and the missing-file fallback.

## Out of scope

Album gain; a limiter; levelling FLAC playback (the phone has none);
pick-a-candidate downloads (separate idea, not requested).
