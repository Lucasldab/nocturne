package io.nocturne.phone.data.catalog

import io.nocturne.phone.data.db.NocturneDatabase
import java.io.InputStream

/**
 * Catalog importer stub — full implementation lands in Plan 04-03 Task 3.
 * This stub exists only so Plan 04-03 Task 2 (AppContainer wiring) can
 * compile without forward-referencing an unimplemented class. Task 3
 * replaces this file in full.
 */
data class ImportResult(
    val tracksImported: Int,
    val albumsImported: Int,
    val artistsImported: Int,
    val genresImported: Int,
    val residentMarked: Int,
    val durationMs: Long,
)

enum class Stage { PARSING_CATALOG, INSERTING_TRACKS, DERIVING_GROUPS, INSERTING_GROUPS, APPLYING_MANIFEST, DONE }

class CatalogImporter(@Suppress("unused") private val db: NocturneDatabase) {
    @Suppress("unused")
    suspend fun importAll(
        catalogIn: InputStream,
        manifestIn: InputStream? = null,
        progress: (Stage, Int, Int) -> Unit = { _, _, _ -> },
    ): ImportResult {
        throw NotImplementedError("CatalogImporter implementation lands in Plan 04-03 Task 3")
    }
}
