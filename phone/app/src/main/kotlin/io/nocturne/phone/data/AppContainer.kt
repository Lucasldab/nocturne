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
 */
class AppContainer(applicationContext: Context) {
    val appContext: Context = applicationContext

    val db: NocturneDatabase by lazy {
        Room.databaseBuilder(
            applicationContext,
            NocturneDatabase::class.java,
            "nocturne.db",
        )
            // Phase 4 ships v1; Phase 5+ will introduce explicit Migration objects
            // when the schema bumps. fallbackToDestructiveMigration(false) makes
            // accidental no-migration drift fail loudly instead of nuking user data.
            .fallbackToDestructiveMigration(false)
            .build()
    }

    val syncPrefs: SyncPrefs by lazy { SyncPrefs(applicationContext) }
    val importer: CatalogImporter by lazy { CatalogImporter(db) }
}
