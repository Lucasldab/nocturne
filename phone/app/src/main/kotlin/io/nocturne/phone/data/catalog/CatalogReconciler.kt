package io.nocturne.phone.data.catalog

import android.content.Context
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.prefs.SyncPrefs
import io.nocturne.phone.player.QueueRepository
import io.nocturne.phone.player.SavedQueue
import java.time.Instant

/**
 * Re-runs CatalogImporter against catalog.json + manifest.json from the
 * SAF-picked metadata tree. Fixes the gap left by ManifestReconciler, which
 * only flips `isResident` on rows that already exist — meaning tracks added
 * to the catalog after first install would never show up in the app.
 *
 * Mtime-gated: callers probe [catalogMtime] cheaply each tick and only invoke
 * [reconcile] when it advances. Importer is idempotent (deleteAll + insert
 * under one transaction) so re-running with an unchanged catalog is safe but
 * wasteful — hence the gate.
 *
 * Residency is restamped from manifest.json during the same import pass, so
 * a successful reconcile subsumes ManifestReconciler for that tick.
 */
object CatalogReconciler {

    /**
     * Returns the catalog's lastModified() timestamp on success (callers track
     * it for change detection), or null on any failure. The manifest is read
     * alongside if present; missing manifest is non-fatal (cold-start case).
     */
    suspend fun reconcile(
        ctx: Context,
        metaTreeUri: String,
        db: NocturneDatabase,
        importer: CatalogImporter,
        syncPrefs: SyncPrefs,
        queueRepository: QueueRepository? = null,
    ): Long? = runCatching {
        val tree = DocumentFile.fromTreeUri(ctx, metaTreeUri.toUri())
            ?: return@runCatching null
        val catalogFile = tree.findFile("catalog.json") ?: return@runCatching null
        val manifestFile = tree.findFile("manifest.json")
        val mtime = catalogFile.lastModified()

        val resolver = ctx.contentResolver
        val result = resolver.openInputStream(catalogFile.uri)?.use { catIn ->
            val manIn = manifestFile?.let { resolver.openInputStream(it.uri) }
            try {
                importer.importAll(catIn, manIn)
            } finally {
                manIn?.close()
            }
        } ?: return@runCatching null

        syncPrefs.setLastImportAt(Instant.now().toString())

        // Catalog re-import wipes + re-inserts every tracks row. The persisted
        // queue (QueueRepository) holds mediaIds (sha256s) that ExoPlayer
        // re-resolves to MediaItems via trackDao on PlaybackResumption — and
        // the resolved path/format may have shifted (e.g. transcode flipped
        // resident FLAC → opus, deleting the old file). If the queue still
        // points at the old paths, ExoPlayer hits ERROR_CODE_IO_FILE_NOT_FOUND
        // and the auto-skip listener trips an infinite "skip skip skip" loop.
        // Clearing here trades a one-time queue-resumption loss for permanent
        // immunity from the trap.
        queueRepository?.runCatching { saveQueue(SavedQueue.EMPTY) }?.onFailure {
            android.util.Log.w("CatalogReconciler", "queue clear failed: ${it.message}")
        }

        android.util.Log.i(
            "CatalogReconciler",
            "reimported: tracks=${result.tracksImported} albums=${result.albumsImported} " +
                "resident=${result.residentMarked} ${result.durationMs}ms (mtime=$mtime)",
        )
        mtime
    }.onFailure {
        android.util.Log.w("CatalogReconciler", "reconcile failed: ${it.message}")
    }.getOrNull()

    /** Cheap mtime probe — used by the poll loop to skip work when unchanged. */
    fun catalogMtime(ctx: Context, metaTreeUri: String): Long? = runCatching {
        DocumentFile.fromTreeUri(ctx, metaTreeUri.toUri())
            ?.findFile("catalog.json")
            ?.lastModified()
    }.getOrNull()
}
