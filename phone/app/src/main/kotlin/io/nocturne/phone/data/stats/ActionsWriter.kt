package io.nocturne.phone.data.stats

import androidx.core.net.toUri
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.flow.first

/**
 * Long-press action emitter — fire-and-forget JSONL writes for the unsync
 * and delete actions backing the phone's track/album bottom-sheet menu.
 *
 * Unlike PinsWriter / LikesWriter / StatsWriter (which drain a Room table
 * to JSONL), this is a direct passthrough: the UI calls emitUnsync /
 * emitDelete with the target id + unit, and we append one JSONL line to
 * `actions-phone-<deviceid>.jsonl` at the top level of metaDir.
 *
 * No persistence on the phone side. The action is whatever the daemon
 * decides to do once Syncthing carries the line over and ingest dispatches
 * to actions.c (idempotent on re-ingest).
 */
class ActionsWriter(
    private val syncPrefs: SyncPrefs,
    private val fileWriter: JsonlFileWriter,
) {
    suspend fun emit(unit: String, id: String, action: String): Boolean {
        require(unit == "track" || unit == "album") {
            "unit must be 'track' or 'album', got: $unit"
        }
        require(action == "unsync" || action == "delete") {
            "action must be 'unsync' or 'delete', got: $action"
        }
        val treeUriStr = syncPrefs.metaTreeUri.first() ?: return false
        val treeUri = treeUriStr.toUri()
        val deviceId = syncPrefs.deviceId()
        val event = ActionEvent(
            v = 1,
            ts = System.currentTimeMillis(),
            unit = unit,
            id = id,
            action = action,
        )
        return fileWriter.appendLine(
            treeUri = treeUri,
            relativePath = "actions-phone-$deviceId.jsonl",
            event = event,
            serializer = ActionEvent.serializer(),
        )
    }

    suspend fun emitUnsyncTrack(sha: String) = emit("track", sha, "unsync")
    suspend fun emitUnsyncAlbum(albumId: String) = emit("album", albumId, "unsync")
    suspend fun emitDeleteTrack(sha: String) = emit("track", sha, "delete")
    suspend fun emitDeleteAlbum(albumId: String) = emit("album", albumId, "delete")
}
