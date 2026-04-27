package io.nocturne.phone.data

import android.content.Context
import androidx.room.Room
import io.nocturne.phone.data.catalog.CatalogImporter
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.prefs.SyncPrefs

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
            // Phase 4 only: Room is entirely derived state from catalog.json,
            // so destructive migration on schema bumps is acceptable — the next
            // import re-populates everything. Phase 5+ introduces user-generated
            // state (queue, last-played, pin writes) and must switch to explicit
            // Migration objects.
            .fallbackToDestructiveMigration(true)
            .build()
    }

    val syncPrefs: SyncPrefs by lazy { SyncPrefs(applicationContext) }
    val importer: CatalogImporter by lazy { CatalogImporter(db) }
}
