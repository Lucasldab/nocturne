# transcode — lossy resident copies

## Why

Bluetooth audio re-encodes everything to SBC/AAC at the link layer, so the FLAC
bytes shipped from desktop → phone → earbud are pure waste. Replacing
`resident/X.flac` with a lossy copy frees Syncthing bandwidth and resident-cap
storage, which translates directly into more pinned tracks at the same cap.

Measured savings on a single FLAC (HAARPER — Bungee Gum, 13.2 MB):

| format     | size   | % of FLAC | tags preserved | embedded art |
|------------|--------|-----------|----------------|--------------|
| Opus 128k  | 1.7 MB | 13.1%     | yes            | **no (v1)**  |
| AAC 256k   | 3.7 MB | 28.0%     | yes            | yes          |

At a 12 GiB phone cap: ~150 FLAC residents → ~1500 Opus residents, ~600 AAC.

## Status

**v1 (this commit)**: standalone CLI for hand-testing. No daemon integration
yet — `rotate.c` still hardlinks FLACs into `resident/`. The transcoder is a
pure tool you can invoke against any source file.

```sh
nocturned transcode <src> <dst> [--format opus|aac] [--bitrate N]
```

Defaults: `--format opus --bitrate 128`.

Config block (used by future rotate integration; ignored by the CLI unless
flags are absent):

```toml
[transcode]
enabled = false
format  = "opus"
bitrate_kbps = 128
```

## Embedded art caveat (Opus)

Ogg/Opus rejects the MJPEG attached_pic stream that FLACs carry. Opus art
must be packed into a `METADATA_BLOCK_PICTURE` Vorbis comment, which ffmpeg
can't do in a single pass. v1 drops the picture stream for Opus output
(`-vn`). The phone falls back to album-folder art / generic icon.

AAC/M4A keeps the cover transparently via `-c:v copy -disposition:v
attached_pic`.

v2 path for Opus art: post-process with `opustags --add
METADATA_BLOCK_PICTURE=...` after the main encode, or do a two-step ffmpeg
(extract cover → encode audio → reattach). Not blocking.

## Roadmap to integration

The hard part is rotate's destructive contract: `archive/X.flac` →
`resident/X.flac` is currently a `link()` + `unlink()` pair (effectively a
move, not a copy). To transcode, we must KEEP the archive copy. Options:

1. **Two-tier storage**: rotate stops moving files. archive/ becomes the
   master, resident/ becomes a transcode cache. Promote = transcode FLAC →
   resident/X.opus (no archive change). Demote = unlink resident/X.opus.
   Catalog publish must emit `path = resident/X.opus`, `format = opus`,
   `size_bytes = transcoded` for resident tracks; `path = archive/X.flac`,
   `format = flac`, original size for non-resident.

2. **Track-id stability**: today's track id = sha256 of audio payload.
   Transcoding changes the payload → would break pin/like linkage. Resolution:
   keep id pinned to FLAC sha; transcoded resident files carry their parent's
   id in some sidecar. Cleanest is a new `resident_transcode (sha,
   transcode_path, transcode_size, transcode_format, transcode_mtime)` table
   joined at publish time; `tracks.path` always reflects the archive.

3. **One-time migration**: existing residents are FLAC hardlinks with the
   archive copy already gone (rotate moved them). Migration job:
   - For each resident FLAC: hardlink it back into archive (free) — archive
     copy restored.
   - Transcode resident FLAC → resident.opus.
   - Drop the resident FLAC hardlink.
   - Update `resident_transcode` for each touched sha.
   - Rebuild manifest + catalog.

Until that lands, this CLI is the way to bench-test quality: pick a few
tracks across genres, transcode at 96/128/160/192k Opus, A/B with the FLAC
on your actual earbuds. Decide the bitrate before committing to the
migration.
