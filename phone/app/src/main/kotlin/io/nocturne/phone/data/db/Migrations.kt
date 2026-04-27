package io.nocturne.phone.data.db

import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase

/**
 * v=2 -> v=3: introduce the `pins` table.
 *
 * Phase 5 reverses the Phase-4 destructive-migration policy. Pins are
 * user-generated state; losing them silently on schema bump would defeat
 * PLAY-10. See 04-05-SUMMARY.md "Schema bump v=1 -> v=2" for the original
 * Phase-4 carry-forward note that mandated this switch.
 */
val MIGRATION_2_3 = object : Migration(2, 3) {
    override fun migrate(database: SupportSQLiteDatabase) {
        database.execSQL(
            """
            CREATE TABLE IF NOT EXISTS `pins` (
                `id` TEXT NOT NULL,
                `unit` TEXT NOT NULL,
                `pinnedAt` INTEGER NOT NULL,
                `synced` INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(`id`)
            )
            """.trimIndent(),
        )
    }
}

/**
 * v=3 -> v=4 (Phase 6 / D-17 / D-19): single atomic migration adding
 *   (a) `pinned` column to existing `pins` table — Phase 6 unpin tombstone support
 *   (b) new `likes` table — Phase 6 LikesWriter drain pattern (Option A)
 *
 * Why a single migration (D-19): atomic schema bump means one migration test
 * instead of two; LWW semantics align with the spec (row presence + column
 * flip vs row absence).
 *
 * Existing pins rows default `pinned = 1` (true) — they were pinned at the
 * time of the row insert; the new column merely makes that explicit. This
 * preserves PLAY-10 state across the bump.
 *
 * Existing pin rows that were already `synced = 1` stay synced (no re-emit).
 * Future unpin actions flip `pinned = 0` AND reset `synced = 0` so the
 * PinsWriter drains the tombstone.
 */
val MIGRATION_3_4 = object : Migration(3, 4) {
    override fun migrate(database: SupportSQLiteDatabase) {
        // (a) Pins: add the pinned column, default 1 for existing rows.
        database.execSQL(
            "ALTER TABLE pins ADD COLUMN pinned INTEGER NOT NULL DEFAULT 1",
        )

        // (b) Likes: new table for Option A drain pattern. Composite PK so
        // the same id can appear once for unit='track' and once for unit='album'
        // (e.g. user likes both an album and one of its tracks).
        database.execSQL(
            """
            CREATE TABLE IF NOT EXISTS `likes` (
                `id` TEXT NOT NULL,
                `unit` TEXT NOT NULL,
                `liked` INTEGER NOT NULL,
                `likedAt` INTEGER NOT NULL,
                `synced` INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(`id`, `unit`)
            )
            """.trimIndent(),
        )
    }
}
