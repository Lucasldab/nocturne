-- nocturne schema v8 — orphan-row cleanup on track deletion.
--
-- pins, likes, and unsync_overrides key off a track sha256 but have NO
-- foreign key to tracks (pins/likes never did; likes lost its FK in the
-- 0004 DROP+CREATE; unsync_overrides was added without one). So
-- `DELETE FROM tracks` — run from the delete-everywhere CLI/ingest action,
-- the scan unseen-sweep (track_repo DELETE_UNSEEN_SQL), and the scan
-- content-changed re-hash path — does NOT cascade to them. A track that is
-- pinned/liked/unsynced when its row is deleted leaves an orphan row that
-- the phone keeps rendering as pinned forever, and that re-suppresses a
-- re-imported sha via unsync_overrides. (Orphan-pin bug, found 2026-06-13.)
--
-- The earlier point-fix only patched delete_track_everywhere in actions.c,
-- missing the scan paths. This trigger is the single source of truth: it
-- fires for EVERY tracks-row deletion regardless of code path, in the same
-- implicit transaction as the DELETE, so the cleanup is atomic and can
-- never be forgotten by a new call site. Triggers fire independent of
-- PRAGMA foreign_keys, so this also closes the "FK was never declared" gap
-- without a table rebuild.

CREATE TRIGGER IF NOT EXISTS trg_tracks_after_delete_cleanup
AFTER DELETE ON tracks
BEGIN
    DELETE FROM pins             WHERE unit = 'track' AND id = OLD.sha256;
    DELETE FROM likes            WHERE unit = 'track' AND id = OLD.sha256;
    DELETE FROM unsync_overrides WHERE sha256 = OLD.sha256;
END;

-- Backfill: clear orphans already accumulated before the trigger existed.
-- (pins/likes were cleaned by hand on 2026-06-13; unsync_overrides had 3
-- stragglers. Idempotent — safe if already empty.)
DELETE FROM pins             WHERE unit = 'track'
                              AND id NOT IN (SELECT sha256 FROM tracks);
DELETE FROM likes            WHERE unit = 'track'
                              AND id NOT IN (SELECT sha256 FROM tracks);
DELETE FROM unsync_overrides WHERE sha256 NOT IN (SELECT sha256 FROM tracks);

PRAGMA user_version = 8;
