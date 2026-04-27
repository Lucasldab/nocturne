package io.nocturne.phone.data

import android.content.Context
import androidx.room.Room
import io.nocturne.phone.data.catalog.CatalogImporter
import io.nocturne.phone.data.db.MIGRATION_2_3
import io.nocturne.phone.data.db.MIGRATION_3_4
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.prefs.SyncPrefs
import io.nocturne.phone.data.stats.JsonlFileWriter
import io.nocturne.phone.data.stats.StatsWriter
import io.nocturne.phone.player.QueueRepository

/**
 * Manual DI container — single instance held by NocturneApp.
 *
 * Wiring points are few (database, prefs, importer, plus future player +
 * stats wires in Phases 5/6) and the dependency graph is acyclic and
 * shallow, so a Hilt/Dagger framework is unnecessary overhead. CONTEXT
 * decision: "NO Hilt/Dagger (manual DI)."
 *
 * The optional `dbOverride` parameter is a test seam: Robolectric tests pass
 * a `Room.inMemoryDatabaseBuilder(...).build()` instance so they don't touch
 * the real `nocturne.db` file.
 */
class AppContainer(
    applicationContext: Context,
    private val dbOverride: NocturneDatabase? = null,
) {
    val appContext: Context = applicationContext

    val db: NocturneDatabase by lazy {
        dbOverride ?: Room.databaseBuilder(
            applicationContext,
            NocturneDatabase::class.java,
            "nocturne.db",
        )
            // Phase 5+: pins are user-generated state. Use explicit Migration objects;
            // destructive migration would silently wipe PLAY-10 data on schema bump.
            // Phase 6 (D-19): MIGRATION_3_4 atomically adds the `pinned` column on
            // pins + creates the new `likes` table.
            .addMigrations(MIGRATION_2_3, MIGRATION_3_4)
            .build()
    }

    val syncPrefs: SyncPrefs by lazy { SyncPrefs(applicationContext) }
    val importer: CatalogImporter by lazy { CatalogImporter(db) }

    // Phase 5 (PLAY-04): queue persistence across process death + reboot.
    // PlaybackService accesses this via (application as NocturneApp).container.queueRepository.
    val queueRepository: QueueRepository by lazy { QueueRepository(applicationContext) }

    // Phase 6 (D-01): shared SAF append + fsync writer used by StatsWriter / LikesWriter / PinsWriter.
    val jsonlFileWriter: JsonlFileWriter by lazy {
        JsonlFileWriter(applicationContext, syncPrefs)
    }

    // Phase 6 (06-02 / STATS-01 / STATS-02 / D-23 / D-24): play/skip JSONL emitter.
    // Wraps jsonlFileWriter with `stats/phone-<deviceid>.jsonl` path resolution.
    val statsWriter: StatsWriter by lazy { StatsWriter(syncPrefs, jsonlFileWriter) }
}
