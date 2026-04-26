-- nocturne schema v4 — Phase 7 desktop ingester storage.
--
-- Adds ingest_offsets (per-source-file byte offset tracking), reshapes
-- likes to (unit, id, liked, ts) so album-level likes are addressable,
-- and extends plays with the locked-contract columns (ts, event_kind,
-- source_path) without breaking existing resolver queries.

-- Extend plays additively. Resolver continues reading played_at / is_skip / src.
-- Ingester writes ALL six columns: legacy three (for resolver) + new three (for
-- audit / future analytics).
ALTER TABLE plays ADD COLUMN ts            INTEGER NOT NULL DEFAULT 0;     -- unix-ms (canonical event ts)
ALTER TABLE plays ADD COLUMN event_kind    TEXT    NOT NULL DEFAULT 'play';-- 'play' | 'skip'
ALTER TABLE plays ADD COLUMN source_path   TEXT    NOT NULL DEFAULT '';    -- e.g. 'stats/phone-LK4F.jsonl'
CREATE INDEX IF NOT EXISTS idx_plays_ts ON plays(ts);

-- ingest_offsets: per-source-file byte offset consumed by `nocturned ingest`.
-- Re-running ingest on an unchanged input set is a no-op via this table.
-- `path` is RELATIVE to meta_dir so the table is portable across moves.
CREATE TABLE IF NOT EXISTS ingest_offsets (
    path           TEXT PRIMARY KEY NOT NULL,    -- e.g. 'stats/phone-LK4F.jsonl'
    offset         INTEGER NOT NULL DEFAULT 0,   -- bytes consumed (always at a newline boundary)
    last_event_ts  INTEGER NOT NULL DEFAULT 0,   -- highest ts seen in this file (unix-ms)
    updated_at     INTEGER NOT NULL DEFAULT 0    -- unix-ms when this row last advanced
);

-- Reshape likes: drop and recreate. Phase-7-or-earlier likes table is
-- empty (no ingest has run yet); the only writer was Phase 7 itself.
DROP TABLE IF EXISTS likes;

CREATE TABLE likes (
    unit       TEXT NOT NULL,                    -- 'track' | 'album'
    id         TEXT NOT NULL,                    -- sha256 (track) or album-key (album)
    liked      INTEGER NOT NULL,                 -- 0 | 1 (LWW from JSONL)
    ts         INTEGER NOT NULL,                 -- unix-ms (event ts; LWW key)
    PRIMARY KEY (unit, id)
);
CREATE INDEX IF NOT EXISTS idx_likes_unit ON likes(unit);

-- Pins are already (unit, id, pinned, updated_at) from 0002. Add `ts INTEGER`
-- column for parity with likes; ingester populates both ts and updated_at
-- (legacy updated_at kept so any future code reads keep working).
ALTER TABLE pins ADD COLUMN ts INTEGER NOT NULL DEFAULT 0;

PRAGMA user_version = 4;
