package io.nocturne.phone.data.stats

import androidx.core.net.toUri
import io.nocturne.phone.data.db.dao.DownloadDao
import io.nocturne.phone.data.db.entity.DownloadEntity
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.flow.first

/**
 * Drains pending [DownloadEntity] rows to `downloads-phone-<deviceid>.jsonl`
 * at the top level of metaDir. Pattern mirrors [PinsWriter] — iterate
 * `synced = 0` rows, append one [DownloadRequestEvent] line per row,
 * flip the row to `synced = 1` / `state = "queued"` on success.
 *
 * Triggered from the `$ submit` button in the Sync screen so the user gets
 * immediate JSONL emission, and again on PlaybackService.onCreate so any
 * row that didn't drain on its initial submit gets retried at next launch.
 */
class DownloadsWriter(
    private val downloadDao: DownloadDao,
    private val syncPrefs: SyncPrefs,
    private val fileWriter: JsonlFileWriter,
) {
    suspend fun drain(): Int {
        val treeUriStr = syncPrefs.metaTreeUri.first() ?: return 0
        val treeUri = treeUriStr.toUri()
        val deviceId = syncPrefs.deviceId()
        val pending: List<DownloadEntity> = downloadDao.unsyncedList()
        var emitted = 0
        for (row in pending) {
            val event = DownloadRequestEvent(
                v = 1,
                id = row.id,
                query = row.query,
                ts = row.requestedAt,
            )
            val ok = fileWriter.appendLine(
                treeUri = treeUri,
                relativePath = "downloads-phone-$deviceId.jsonl",
                event = event,
                serializer = DownloadRequestEvent.serializer(),
            )
            if (ok) {
                downloadDao.markQueued(row.id)
                emitted++
            }
        }
        return emitted
    }
}
