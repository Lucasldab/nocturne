-- nocturne schema v3 — path-layout selective-sync state.
--
-- residency_state is the diff target for `nocturned rotate` (plan 03-02).
-- After `nocturned migrate --apply`, every track is in 'archive'.
-- After `nocturned rotate`, tracks selected by manifest_current move to
-- 'resident' on disk and the row here is updated.

CREATE TABLE IF NOT EXISTS residency_state (
    sha256      TEXT PRIMARY KEY NOT NULL,
    location    TEXT NOT NULL CHECK (location IN ('resident', 'archive')),
    updated_at  TEXT NOT NULL,           -- ISO-8601 UTC
    FOREIGN KEY (sha256) REFERENCES tracks(sha256) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_residency_location ON residency_state(location);

-- last_rotation_at is a single row in manifest_meta (key='last_rotation_at')
-- written by `nocturned rotate` (plan 03-02). Not created here as a row;
-- the table already exists from 0002_resolver.sql. Plan 03-02's rotate.c
-- handles the upsert.

PRAGMA user_version = 3;
