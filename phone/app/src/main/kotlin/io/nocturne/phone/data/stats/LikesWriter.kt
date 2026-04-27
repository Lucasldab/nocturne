package io.nocturne.phone.data.stats

import android.net.Uri
import io.nocturne.phone.data.db.dao.LikeDao
import io.nocturne.phone.data.db.entity.LikeEntity
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.flow.first

/**
 * Phase 6 (STATS-03 / D-15 / D-25): drains the `likes` table to JSONL.
 *
 * Per docs/jsonl-spec.md §3, the file is `likes-phone-<deviceid>.jsonl` at
 * the TOP LEVEL of metaDir (NOT under stats/).
 *
 * Two entry points:
 *   - `record(id, unit, liked)`: upsert + drain — atomic from the UI's
 *     point of view; the caller does not need a separate drain step.
 *   - `drain()`: explicit drain (call from PlaybackService.onCreate or future
 *     WorkManager fallback to cover events created while service was dead).
 */
class LikesWriter(
    private val likeDao: LikeDao,
    private val syncPrefs: SyncPrefs,
    private val fileWriter: JsonlFileWriter,
    private val nowMs: () -> Long = { System.currentTimeMillis() },
) {
    suspend fun record(id: String, unit: String, liked: Boolean): Boolean {
        require(unit == "track" || unit == "album") {
            "unit must be 'track' or 'album', got: $unit"
        }
        likeDao.upsert(
            LikeEntity(
                id = id,
                unit = unit,
                liked = liked,
                likedAt = nowMs(),
                synced = false,
            ),
        )
        return drain() > 0 || syncPrefs.metaTreeUri.first() == null
        // If metaTreeUri is null, the row stays in DB; a later drain (when the
        // user grants the SAF tree URI in first-run) will pick it up. We return
        // true in that case so the UI does not show an error — the local row
        // is durable and the JSONL emission is deferred, not lost.
    }

    suspend fun drain(): Int {
        val treeUriStr = syncPrefs.metaTreeUri.first() ?: return 0
        val treeUri = Uri.parse(treeUriStr)
        val deviceId = syncPrefs.deviceId()
        val pending: List<LikeEntity> = likeDao.unsyncedList()
        var emitted = 0
        for (like in pending) {
            val event = LikeEvent(
                v = 1,
                ts = like.likedAt,
                unit = like.unit,
                id = like.id,
                liked = like.liked,
            )
            val ok = fileWriter.appendLine(
                treeUri = treeUri,
                relativePath = "likes-phone-$deviceId.jsonl",
                event = event,
                serializer = LikeEvent.serializer(),
            )
            if (ok) {
                likeDao.markSynced(like.id, like.unit)
                emitted++
            }
        }
        return emitted
    }
}
