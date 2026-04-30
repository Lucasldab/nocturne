package io.nocturne.phone.data.sync

import android.content.Context
import android.provider.DocumentsContract
import androidx.core.net.toUri
import io.nocturne.phone.data.db.NocturneDatabase
import io.nocturne.phone.data.db.entity.TrackEntity
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext

/**
 * Read-only progress probe for pinned-but-not-yet-resident tracks.
 *
 * Mechanism: Syncthing-Fork stages incoming files as
 * `.syncthing.<basename>.tmp` in the destination directory and renames
 * to the final name on completion. Stat-ing the temp file via SAF gives
 * the in-flight byte count; dividing by the catalog's `sizeBytes` gives
 * a 0..1 fraction.
 *
 * Returns null progress when:
 *  - musicTreeUri not configured yet (pre-onboarding)
 *  - neither the temp file nor the final file is queryable (transfer
 *    not started, or Syncthing isn't running)
 *  - sizeBytes <= 0 (catalog row missing the metadata)
 *
 * Returns 1.0f when the final file is present at full size — the
 * residency reconcile hasn't flipped isResident yet but the bytes are
 * fully on-device.
 *
 * Probes three temp-name variants to be robust to Syncthing version
 * drift: `.syncthing.<name>.tmp` (current default), `~syncthing~<name>.tmp`
 * (pre-1.0), and `<name>.tmp` (very old). First one that resolves wins.
 */
class SyncProgressRepository(
    private val appContext: Context,
    private val db: NocturneDatabase,
    private val syncPrefs: SyncPrefs,
) {

    /**
     * Probe progress for every track id in [trackIds]. Caller filters to
     * pinned-not-resident before calling — this method does no DB-side
     * filtering and will happily probe any track id.
     *
     * Result map contains an entry for every input id; value is null when
     * progress can't be determined (treat as "unknown / not started").
     */
    suspend fun probe(trackIds: Collection<String>): Map<String, Float?> = withContext(Dispatchers.IO) {
        if (trackIds.isEmpty()) return@withContext emptyMap()
        val musicTreeStr = syncPrefs.musicTreeUri.first() ?: return@withContext emptyMap()
        val musicTreeUri = musicTreeStr.toUri()
        val treeDocId = DocumentsContract.getTreeDocumentId(musicTreeUri)
        val resolver = appContext.contentResolver

        val rows: List<TrackEntity> = trackIds.chunked(500).flatMap { chunk ->
            db.trackDao().byIds(chunk)
        }
        val out = HashMap<String, Float?>(rows.size)
        for (track in rows) {
            val expected = track.sizeBytes
            if (expected <= 0L) {
                out[track.id] = null
                continue
            }
            val rel = track.path
                .removePrefix("resident/")
                .removePrefix("archive/")
            val slashIdx = rel.lastIndexOf('/')
            val dirRel = if (slashIdx >= 0) rel.substring(0, slashIdx) else ""
            val baseName = if (slashIdx >= 0) rel.substring(slashIdx + 1) else rel

            // Final file at full size → effectively done; residency flip is async.
            val finalSize = statSize(resolver, musicTreeUri, treeDocId, dirRel, baseName)
            if (finalSize != null && finalSize >= expected) {
                out[track.id] = 1f
                continue
            }

            // Probe temp-file variants in order of likelihood.
            val tempNames = listOf(
                ".syncthing.$baseName.tmp",
                "~syncthing~$baseName.tmp",
                "$baseName.tmp",
            )
            var bytes: Long? = null
            for (tempName in tempNames) {
                val sz = statSize(resolver, musicTreeUri, treeDocId, dirRel, tempName)
                if (sz != null) { bytes = sz; break }
            }
            out[track.id] = bytes?.let { (it.toFloat() / expected.toFloat()).coerceIn(0f, 1f) }
        }
        out
    }

    private fun statSize(
        resolver: android.content.ContentResolver,
        musicTreeUri: android.net.Uri,
        treeDocId: String,
        dirRel: String,
        fileName: String,
    ): Long? {
        val docId = if (dirRel.isEmpty()) "$treeDocId/$fileName" else "$treeDocId/$dirRel/$fileName"
        val docUri = DocumentsContract.buildDocumentUriUsingTree(musicTreeUri, docId)
        return try {
            resolver.query(
                docUri,
                arrayOf(DocumentsContract.Document.COLUMN_SIZE),
                null, null, null,
            )?.use { c ->
                if (c.moveToFirst() && !c.isNull(0)) c.getLong(0) else null
            }
        } catch (_: Throwable) {
            null
        }
    }
}
