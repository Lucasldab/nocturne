# transcode — lossy resident copies

## Why

Bluetooth audio re-encodes everything to SBC/AAC at the link layer, so the FLAC
bytes shipped from server → phone → earbud are pure waste. Replacing
`resident/X.flac` with a lossy copy frees Syncthing bandwidth and resident-cap
storage, which translates directly into more pinned tracks at the same cap.

Measured savings on a single FLAC (HAARPER — Bungee Gum, 13.2 MB):

| format     | size   | % of FLAC | tags preserved | embedded art |
|------------|--------|-----------|----------------|--------------|
| Opus 128k  | 1.7 MB | 13.1%     | yes            | **no**       |
| AAC 256k   | 3.7 MB | 28.0%     | yes            | yes          |

At a 12 GiB phone cap: ~150 FLAC residents → ~1500 Opus residents, ~600 AAC.

## Configuration

```toml
[transcode]
enabled = 1            # integer: 1 = on, 0 = off (the default)
format  = "opus"       # or "aac"
bitrate_kbps = 128
level_lufs = -18       # optional, see "Loudness levelling"
```

`enabled` must be an integer. `enabled = true` is rejected by the config parser.

## How it works

Without transcoding, rotate moves a track with `link()` + `unlink()`, so the FLAC
is in `archive/` or `resident/`, never both.

With transcoding on, `archive/` keeps the FLAC and `resident/` holds a lossy
copy. Promoting a track encodes the copy, and demoting it unlinks the copy.

The track id stays the sha256 of the FLAC's audio, so pins and likes survive.
`residency_state` records each copy's path, size and format, and the catalog
publishes the resident copy for tracks on the phone.

## Moving an existing library

To convert a library that already has FLAC residents, run the one-time migration.
It restores the archive hardlinks and encodes each current resident:

```sh
nocturned transcode-migrate              # dry run
nocturned transcode-migrate --apply
```

## Testing a bitrate

The standalone command encodes one file without touching the library. Use it to
compare bitrates on your own earbuds before committing to a migration:

```sh
nocturned transcode <src> <dst> [--format opus|aac] [--bitrate N]
```

Defaults: `--format opus --bitrate 128`.

## Embedded art (Opus)

Ogg/Opus rejects the MJPEG attached_pic stream that FLACs carry, so Opus output
drops the picture stream (`-vn`). The phone uses the `cover.jpg` sidecars that
rotate places next to resident tracks instead.

AAC/M4A keeps the cover via `-c:v copy -disposition:v attached_pic`.

## Loudness levelling

With `[transcode] level_lufs = -18` (integer LUFS; 0 = off), every promote
measures the archive FLAC with ffmpeg's `loudnorm` in analysis mode and
encodes the resident copy through `volume=<gain>dB`:

    gain = target - integrated
    if gain > 0: gain = min(gain, max(0, -1 dBTP - true_peak))   # boosts only
    clamp to [-30, +12]

Per-track, no limiter: a quiet track with sharp peaks is boosted less (or not
at all) rather than compressed, and is never cut. A source that measures as silence, or fails to measure,
is encoded at 0 dB. The FLAC is never modified (its bytes are the track id).

`residency_state.transcode_gain_db` / `transcode_lufs` record what was done;
NULL gain = copy predates levelling. `nocturned transcode-level [--apply]`
re-encodes exactly those rows in place (atomic rename) and updates their
size, so the phone re-syncs them. It takes the daemon lock: stop the watcher
first. Design: `docs/superpowers/specs/2026-10-10-loudness-and-download-feedback-design.md`.
