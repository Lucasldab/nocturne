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
