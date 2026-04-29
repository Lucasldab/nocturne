-- nocturne schema v5 — resident-side transcode metadata.
--
-- When [transcode] is enabled in config.toml, rotate's promote path no
-- longer hardlinks the archive FLAC into resident/. Instead it transcodes
-- to Opus (or AAC) and writes the lossy copy to resident/<rel>.<ext>. The
-- archive FLAC stays untouched as the canonical source of truth — track id
-- (= sha256 of archive payload) remains stable across re-encode cycles.
--
-- These columns store WHAT was written to disk in resident/ (path / size /
-- format) so catalog publish can emit transcoded metadata without re-stat'ing
-- the file. NULL when the residency_state row is at location='archive', or
-- when transcode is disabled (resident is just a hardlink of archive).

ALTER TABLE residency_state ADD COLUMN transcode_path TEXT;
ALTER TABLE residency_state ADD COLUMN transcode_size_bytes INTEGER;
ALTER TABLE residency_state ADD COLUMN transcode_format TEXT;

PRAGMA user_version = 5;
