-- nocturne schema v2 — resolver/publisher tables.
--
-- Adds plays/likes/pins (populated by Phase 7 ingest) plus the
-- manifest_current and manifest_meta tables the resolver writes into.

CREATE TABLE IF NOT EXISTS plays (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    sha256      TEXT NOT NULL,
    played_at   TEXT NOT NULL,           -- ISO-8601 UTC
    played_ms   INTEGER,
    is_skip     INTEGER NOT NULL DEFAULT 0,
    src         TEXT NOT NULL,           -- "phone-LK4F" / "desktop-A2BR" / ...
    FOREIGN KEY (sha256) REFERENCES tracks(sha256) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_plays_sha256_at ON plays(sha256, played_at DESC);
CREATE INDEX IF NOT EXISTS idx_plays_src ON plays(src);

CREATE TABLE IF NOT EXISTS likes (
    sha256      TEXT PRIMARY KEY NOT NULL,
    liked       INTEGER NOT NULL,         -- 0 unliked / 1 liked (LWW from JSONL)
    updated_at  TEXT NOT NULL,
    FOREIGN KEY (sha256) REFERENCES tracks(sha256) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS pins (
    unit        TEXT NOT NULL,           -- "track" / "album"
    id          TEXT NOT NULL,           -- sha256 (track) or album_artist|album (album)
    pinned      INTEGER NOT NULL,        -- 0 unpinned / 1 pinned (LWW)
    updated_at  TEXT NOT NULL,
    PRIMARY KEY (unit, id)
);

CREATE TABLE IF NOT EXISTS manifest_current (
    sha256      TEXT PRIMARY KEY NOT NULL,
    buckets_csv TEXT NOT NULL,           -- comma-joined bucket names
    size_bytes  INTEGER NOT NULL,
    FOREIGN KEY (sha256) REFERENCES tracks(sha256) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS manifest_meta (
    key         TEXT PRIMARY KEY NOT NULL,
    value       TEXT NOT NULL
);

PRAGMA user_version = 2;
