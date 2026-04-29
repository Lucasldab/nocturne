-- nocturne schema v6 — Weekly Discovery picks.
--
-- Stores the 20-track weekly selection produced by `nocturned discover`.
-- Re-picked every Monday 00:00 (systemd timer); the resolver's new
-- `weekly_discovery_picks` source reads from here.
--
-- Schema:
--   sha256:      track id (FK to tracks)
--   week_start:  YYYY-MM-DD of the Monday this pick is for
--   reason:      'never_played' | 'aged_out' | 'adjacent_to_loved' | 'random'
--   picked_at:   ISO-8601 UTC timestamp of when discover ran
--
-- Old picks aren't deleted — they accumulate so you can browse past weeks.
-- The resolver only reads the most recent week_start group.

CREATE TABLE IF NOT EXISTS weekly_discovery_picks (
    sha256      TEXT NOT NULL,
    week_start  TEXT NOT NULL,        -- 'YYYY-MM-DD' (Monday)
    reason      TEXT NOT NULL,
    picked_at   TEXT NOT NULL,        -- ISO-8601 UTC
    PRIMARY KEY (sha256, week_start),
    FOREIGN KEY (sha256) REFERENCES tracks(sha256) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_weekly_discovery_week
    ON weekly_discovery_picks(week_start);

PRAGMA user_version = 6;
