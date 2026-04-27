package io.nocturne.phone.data.stats

import android.net.Uri
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.flow.first

/**
 * Phase 6 (STATS-01 / STATS-02 / D-23 / D-24): emits one StatsEvent line to
 *   `<metaTreeUri>/stats/phone-<deviceid>.jsonl`
 *
 * Hosted from `PlaybackService.serviceScope` (FGS scope — Doze-immune for file
 * writes per RESEARCH.md Pitfall 8). Delegates byte-shape + fsync discipline
 * to the shared `JsonlFileWriter` (which also updates SyncPrefs.lastStatsSyncAt
 * on every successful append — D-26).
 *
 * Returns false when `metaTreeUri` is not yet provisioned (first run pre-import)
 * — the caller drops the event silently in that case.
 */
class StatsWriter(
    private val syncPrefs: SyncPrefs,
    private val fileWriter: JsonlFileWriter,
) {
    suspend fun record(
        track: String,
        playedMs: Long,
        durationMs: Long,
        kind: String,
        tsMs: Long,
    ): Boolean {
        require(kind == "play" || kind == "skip") {
            "kind must be 'play' or 'skip', got: $kind"
        }
        val treeUriStr = syncPrefs.metaTreeUri.first() ?: return false
        val deviceId = syncPrefs.deviceId()
        val event = StatsEvent(
            v = 1,
            ts = tsMs,
            kind = kind,
            track = track,
            playedMs = playedMs,
            durationMs = durationMs,
        )
        return fileWriter.appendLine(
            treeUri = Uri.parse(treeUriStr),
            relativePath = "stats/phone-$deviceId.jsonl",
            event = event,
            serializer = StatsEvent.serializer(),
        )
    }
}
