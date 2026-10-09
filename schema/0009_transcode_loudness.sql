-- nocturne schema v9 — per-track loudness levelling of resident transcodes.
--
-- rotate measures each source with ffmpeg loudnorm (analysis only) and bakes
-- a gain into the lossy copy, so the phone plays every track near the
-- [transcode] level_lufs target. The FLAC is never touched: retagging it would
-- change the track id.
--
-- transcode_gain_db: gain applied to the resident copy. NULL = not levelled
--   (written before this feature, or levelling off) — `nocturned
--   transcode-level --apply` re-encodes exactly those rows.
-- transcode_lufs: measured integrated loudness of the source; NULL when the
--   measurement failed (the copy was then encoded at 0 dB).

ALTER TABLE residency_state ADD COLUMN transcode_gain_db REAL;
ALTER TABLE residency_state ADD COLUMN transcode_lufs REAL;

PRAGMA user_version = 9;
