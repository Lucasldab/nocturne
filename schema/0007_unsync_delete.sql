-- nocturne schema v7 — unsync overrides + delete blacklist.
--
-- Two new tables backing the long-press track/album actions on the phone:
--
--   unsync_overrides:
--     User said "unpin and unload". Resolver consults this and demotes
--     these shas regardless of bucket inclusion, until `until` passes
--     (NULL = forever). Cleared by re-pinning (UI side) or by running
--     `nocturned unsync --clear <sha>`.
--
--   track_blacklist:
--     User said "didn't like it" → files deleted from archive + resident,
--     tracks row dropped. Sha recorded here so:
--       - scan refuses to insert it on future re-imports (e.g., streamrip
--         re-runs the same Spotify CSV)
--       - discover and resolver exclude it from candidate pools
--       - the manifest publisher silently filters it
--     Survives DB schema bumps and library moves.

CREATE TABLE IF NOT EXISTS unsync_overrides (
    sha256       TEXT PRIMARY KEY NOT NULL,
    until        TEXT,                -- ISO-8601 UTC; NULL = indefinite
    added_at     TEXT NOT NULL        -- ISO-8601 UTC
);

CREATE TABLE IF NOT EXISTS track_blacklist (
    sha256       TEXT PRIMARY KEY NOT NULL,
    reason       TEXT NOT NULL,       -- 'didnt_like' | 'manual' | future
    added_at     TEXT NOT NULL        -- ISO-8601 UTC
);

PRAGMA user_version = 7;
