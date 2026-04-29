package io.nocturne.phone.data.stats

import androidx.core.net.toUri
import io.nocturne.phone.data.db.dao.PinDao
import io.nocturne.phone.data.db.entity.PinEntity
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.flow.first

/**
 * Phase 6 (STATS-03 / D-17 / D-25): drains the `pins` table to JSONL.
 *
 * Per docs/jsonl-spec.md §3, the file is `pins-phone-<deviceid>.jsonl` at the
 * TOP LEVEL of metaDir (NOT under stats/).
 *
 * Drain semantics: iterate every row where `synced = 0`, append one PinEvent
 * line per row, mark synced on success. On failure leave synced = 0; the
 * next drain attempt retries.
 *
 * The drain trigger lives in the caller (PlaybackService.onCreate or future
 * WorkManager fallback per D-25). This class is a pure imperative procedure.
 */
class PinsWriter(
    private val pinDao: PinDao,
    private val syncPrefs: SyncPrefs,
    private val fileWriter: JsonlFileWriter,
) {
    suspend fun drain(): Int {
        val treeUriStr = syncPrefs.metaTreeUri.first() ?: return 0
        val treeUri = treeUriStr.toUri()
        val deviceId = syncPrefs.deviceId()
        val pending: List<PinEntity> = pinDao.unsyncedList()
        var emitted = 0
        for (pin in pending) {
            val event = PinEvent(
                v = 1,
                ts = pin.pinnedAt,
                unit = pin.unit,
                id = pin.id,
                pinned = pin.pinned,
            )
            val ok = fileWriter.appendLine(
                treeUri = treeUri,
                relativePath = "pins-phone-$deviceId.jsonl",
                event = event,
                serializer = PinEvent.serializer(),
            )
            if (ok) {
                pinDao.markSynced(pin.id)
                emitted++
            }
        }
        return emitted
    }
}
