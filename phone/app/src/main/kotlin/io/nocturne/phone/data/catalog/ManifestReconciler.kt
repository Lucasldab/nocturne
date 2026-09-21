package io.nocturne.phone.data.catalog

import android.content.Context
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.data.sync.MusicFileProbe
import kotlinx.serialization.json.Json

/**
 * Re-reads the daemon's manifest.json and flips `track.isResident` to match
 * `manifest.resident[]` **intersected with the files actually on this phone**.
 * CatalogImporter only ran on first install, so without this call `isResident`
 * would stay frozen at the original import even though the daemon's manifest
 * keeps changing as new pins land.
 *
 * The manifest alone is not evidence of residency — see [ResidencyResolver].
 * It arrives over `sync-meta` in seconds while the audio crawls over
 * `sync-files`; trusting it queued tracks whose bytes were still in flight.
 * Every declared id is therefore stat-ed before it is marked resident.
 *
 * Idempotent. Manifest parse is cheap (~25 KB, ~169 ids in steady state); the
 * verification pass is one SAF stat per declared id on the caller's
 * dispatcher.
 *
 * Callers:
 *  - AppRoot launches it on first composition (cold-start)
 *  - AppRoot polls manifest.json mtime every 45s while foregrounded and
 *    re-runs on change (warm-start; closes the kill+reopen workaround)
 *  - AppRoot also calls [reverify] on every poll tick, because a file landing
 *    changes residency without touching the manifest's mtime
 *  - SyncScreen "Refresh now" button (manual override)
 */
object ManifestReconciler {

    private val json = Json { ignoreUnknownKeys = true }

    /**
     * Ids from the most recent manifest read, kept so [reverify] can re-check
     * presence without re-parsing. Empty until the first [reconcile].
     */
    @Volatile
    private var declaredIds: List<String> = emptyList()

    /**
     * Residency last written to the DB, so an unchanged verification pass can
     * skip the write entirely. [applyResidency] rewrites every row's
     * `isResident`, which fires Room table-change notifications into every
     * browser Flow; doing that on each 45s tick would recompose the open list
     * (and drop its scroll position) while nothing had actually changed.
     */
    @Volatile
    private var appliedIds: Set<String>? = null

    /**
     * Returns the manifest's lastModified() timestamp on success (so callers
     * can track mtime for change detection), or null on any failure.
     *
     * @param musicTreeUri the phone's music tree. When null (folder not picked
     *        yet) verification is skipped and the manifest is trusted as
     *        before — otherwise a pre-onboarding phone would show an entirely
     *        non-resident library.
     */
    suspend fun reconcile(
        ctx: Context,
        metaTreeUri: String,
        db: NocturneDatabase,
        musicTreeUri: String? = null,
    ): Long? = runCatching {
        val tree = DocumentFile.fromTreeUri(ctx, metaTreeUri.toUri())
            ?: return@runCatching null
        val manifestFile = tree.findFile("manifest.json") ?: return@runCatching null
        val mtime = manifestFile.lastModified()
        val text = ctx.contentResolver.openInputStream(manifestFile.uri)?.use {
            it.readBytes().toString(Charsets.UTF_8)
        } ?: return@runCatching null
        val manifest = json.decodeFromString(ManifestJson.serializer(), text)
        val residentIds = manifest.resident.map { it.id }
        declaredIds = residentIds

        val verifiedIds = verifyAgainstDisk(ctx, db, residentIds, musicTreeUri)
        applyResidency(db, verifiedIds)

        android.util.Log.i(
            "ManifestReconciler",
            "reconciled: ${verifiedIds.size}/${residentIds.size} declared tracks " +
                "present on disk (mtime=$mtime)",
        )
        mtime
    }.onFailure {
        android.util.Log.w("ManifestReconciler", "reconcile failed: ${it.message}")
    }.getOrNull()

    /**
     * Re-stat the current declared set without re-reading the manifest.
     *
     * A file finishing its transfer flips residency but changes neither
     * manifest.json nor catalog.json, so the mtime-driven poll would never
     * notice it. This closes that gap — and the reverse one, where a file is
     * deleted underneath a manifest that still declares it.
     *
     * No-op before the first [reconcile], or when the music folder is unset.
     * Returns the number of tracks now resident, or null if it did not run.
     */
    suspend fun reverify(
        ctx: Context,
        db: NocturneDatabase,
        musicTreeUri: String?,
    ): Int? = runCatching {
        val declared = declaredIds
        if (declared.isEmpty() || musicTreeUri == null) return@runCatching null
        val verifiedIds = verifyAgainstDisk(ctx, db, declared, musicTreeUri)
        applyResidency(db, verifiedIds)
        verifiedIds.size
    }.onFailure {
        android.util.Log.w("ManifestReconciler", "reverify failed: ${it.message}")
    }.getOrNull()

    /**
     * Intersect [residentIds] with what is on disk. Falls back to trusting the
     * manifest when [musicTreeUri] is unset.
     */
    private suspend fun verifyAgainstDisk(
        ctx: Context,
        db: NocturneDatabase,
        residentIds: List<String>,
        musicTreeUri: String?,
    ): List<String> {
        if (residentIds.isEmpty()) return emptyList()
        if (musicTreeUri == null) {
            android.util.Log.w(
                "ManifestReconciler",
                "music tree not configured — trusting manifest without verifying ${residentIds.size} ids",
            )
            return residentIds
        }
        val probe = MusicFileProbe(ctx.contentResolver, musicTreeUri.toUri())
        // Room IN-clause has SQLite parameter limits. Chunk to be safe.
        val rows: List<TrackEntity> = residentIds.chunked(CHUNK).flatMap { batch ->
            db.trackDao().byIds(batch)
        }
        return ResidencyResolver.verify(rows) { track ->
            probe.sizeOf(ResidencyResolver.relPath(track.path))
        }
    }

    private suspend fun applyResidency(db: NocturneDatabase, verifiedIds: List<String>) {
        val next = verifiedIds.toSet()
        // No-op when the set is unchanged — see [appliedIds]. Null means we
        // have not written since process start, so the DB may hold residency
        // from a previous run and must be restamped even if the set matches.
        if (appliedIds == next) return
        db.trackDao().clearAllResident()
        if (verifiedIds.isNotEmpty()) {
            verifiedIds.chunked(CHUNK).forEach { batch ->
                db.trackDao().setResidentFor(batch, true)
            }
        }
        appliedIds = next
    }

    /**
     * Drop the [appliedIds] cache, forcing the next verification pass to write.
     *
     * CatalogImporter runs `deleteAll` + insert and stamps `isResident`
     * straight from `manifest.resident[]`, unverified. If the verified set
     * happened to be unchanged, [applyResidency] would skip its write and
     * leave that unverified stamping in the DB. Callers must invalidate after
     * any catalog re-import.
     */
    fun invalidateResidencyCache() {
        appliedIds = null
    }

    /** Cheap mtime probe — used by the poll loop to skip work when unchanged. */
    fun manifestMtime(ctx: Context, metaTreeUri: String): Long? = runCatching {
        DocumentFile.fromTreeUri(ctx, metaTreeUri.toUri())
            ?.findFile("manifest.json")
            ?.lastModified()
    }.getOrNull()

    private const val CHUNK = 500
}
