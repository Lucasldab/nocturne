package io.nocturne.phone.data.catalog

import android.content.Context
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.prefs.SyncPrefs
import io.nocturne.phone.player.QueueRepository
import io.nocturne.phone.player.SavedQueue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
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
    ): Long? = withContext(Dispatchers.IO) {
        reconcileBlocking(ctx, metaTreeUri, importer, syncPrefs, queueRepository)
    }

    // SAF reads plus a ~1 MB JSON parse: keep them off the main thread.
    private suspend fun reconcileBlocking(
        ctx: Context,
        metaTreeUri: String,
        importer: CatalogImporter,
        syncPrefs: SyncPrefs,
        queueRepository: QueueRepository?,
    ): Long? = runCatching {
        val catalogFile = MetaFiles.find(ctx, metaTreeUri, "catalog.json")
            ?: return@runCatching null
        val manifestFile = MetaFiles.find(ctx, metaTreeUri, "manifest.json")
        val mtime = catalogFile.lastModified

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
    suspend fun catalogMtime(ctx: Context, metaTreeUri: String): Long? = withContext(Dispatchers.IO) {
        runCatching { MetaFiles.find(ctx, metaTreeUri, "catalog.json")?.lastModified }.getOrNull()
    }
}
