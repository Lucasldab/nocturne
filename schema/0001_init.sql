-- nocturne schema v1 — frozen by plan 02-01.
--
-- Every later migration appends; never edit this file once committed.
-- The migration runner reads PRAGMA user_version to decide which
-- migrations to apply; this file ends with `PRAGMA user_version = 1;`.

CREATE TABLE IF NOT EXISTS tracks (
    sha256        TEXT PRIMARY KEY NOT NULL,            -- 64-char hex of audio bytes
    path          TEXT NOT NULL UNIQUE,                  -- absolute path on disk
    mtime_ns      INTEGER NOT NULL,                      -- st_mtim nanoseconds
    size_bytes    INTEGER NOT NULL,
    format        TEXT,                                  -- mp3 / flac / opus / ogg / m4a
    title         TEXT,
    artist        TEXT,                                  -- canonical (multi-value as JSON array string)
    album         TEXT,
    album_artist  TEXT,
    track_number  TEXT,
    disc_number   TEXT,
    year          TEXT,
    genre         TEXT,
    duration_ms   INTEGER,
    tags_status   TEXT NOT NULL DEFAULT 'ok',            -- 'ok' | 'incomplete' | 'parse_failed'
    tag_warning   TEXT,                                  -- nullable; canon-check issue codes (csv)
    date_added    TEXT NOT NULL,                         -- ISO-8601 UTC
    last_seen_at  TEXT NOT NULL                          -- ISO-8601 UTC; updated each scan pass
);

CREATE INDEX IF NOT EXISTS idx_tracks_path  ON tracks(path);
CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_artist, album);

CREATE TABLE IF NOT EXISTS scan_meta (
    library_root  TEXT PRIMARY KEY NOT NULL,
    last_scan_at  TEXT NOT NULL,                         -- ISO-8601 UTC
    files_seen    INTEGER NOT NULL DEFAULT 0,
    files_added   INTEGER NOT NULL DEFAULT 0,
    files_updated INTEGER NOT NULL DEFAULT 0,
    files_removed INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS app_meta (
    key    TEXT PRIMARY KEY NOT NULL,
    value  TEXT NOT NULL
);

PRAGMA user_version = 1;
