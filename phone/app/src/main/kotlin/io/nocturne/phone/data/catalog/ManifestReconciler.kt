package io.nocturne.phone.data.catalog

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import io.nocturne.phone.data.db.NocturneDatabase
import kotlinx.serialization.json.Json

/**
 * Re-reads the daemon's manifest.json and flips `track.isResident` to match
 * `manifest.resident[]`. CatalogImporter only ran on first install, so without
 * this call `isResident` would stay frozen at the original import even though
 * the daemon's manifest keeps changing as new pins land.
 *
 * Idempotent. Cheap (~18 KB JSON, ~125 ids in steady state).
 *
 * Callers:
 *  - AppRoot launches it on first composition (cold-start)
 *  - AppRoot polls manifest.json mtime every 45s while foregrounded and
 *    re-runs on change (warm-start; closes the kill+reopen workaround)
 *  - SyncScreen "Refresh now" button (manual override)
 */
object ManifestReconciler {

    private val json = Json { ignoreUnknownKeys = true }

    /**
     * Returns the manifest's lastModified() timestamp on success (so callers
     * can track mtime for change detection), or null on any failure.
     * Resident-count is logged but not returned — callers don't act on it.
     */
    suspend fun reconcile(
        ctx: Context,
        metaTreeUri: String,
        db: NocturneDatabase,
    ): Long? = runCatching {
        val tree = DocumentFile.fromTreeUri(ctx, Uri.parse(metaTreeUri))
            ?: return@runCatching null
        val manifestFile = tree.findFile("manifest.json") ?: return@runCatching null
        val mtime = manifestFile.lastModified()
        val text = ctx.contentResolver.openInputStream(manifestFile.uri)?.use {
            it.readBytes().toString(Charsets.UTF_8)
        } ?: return@runCatching null
        val manifest = json.decodeFromString(ManifestJson.serializer(), text)
        val residentIds = manifest.resident.map { it.id }
        db.trackDao().clearAllResident()
        if (residentIds.isNotEmpty()) {
            // Room IN-clause has SQLite parameter limits. Chunk to be safe.
            residentIds.chunked(500).forEach { batch ->
                db.trackDao().setResidentFor(batch, true)
            }
        }
        android.util.Log.i(
            "ManifestReconciler",
            "reconciled: ${residentIds.size} resident tracks (mtime=$mtime)",
        )
        mtime
    }.onFailure {
        android.util.Log.w("ManifestReconciler", "reconcile failed: ${it.message}")
    }.getOrNull()

    /** Cheap mtime probe — used by the poll loop to skip work when unchanged. */
    fun manifestMtime(ctx: Context, metaTreeUri: String): Long? = runCatching {
        DocumentFile.fromTreeUri(ctx, Uri.parse(metaTreeUri))
            ?.findFile("manifest.json")
            ?.lastModified()
    }.getOrNull()
}
