package io.nocturne.phone.data.stats

import androidx.core.net.toUri
import io.nocturne.phone.BuildConfig
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.flow.first

/**
 * Friction-log emitter. Direct passthrough like ActionsWriter: no Room table,
 * no retry queue — a failed write returns false and the gap sheet keeps the
 * user's text so nothing typed is lost.
 *
 * Lines go to `gaps-phone-<deviceid>.jsonl` at the top level of metaDir. The
 * daemon's ingest globs never match it; the PM agent reads it over ssh.
 */
class GapsWriter(
    private val syncPrefs: SyncPrefs,
    private val fileWriter: JsonlFileWriter,
) {
    suspend fun emit(text: String, screen: String, track: String?): Boolean {
        val treeUriStr = syncPrefs.metaTreeUri.first() ?: return false
        val event = buildEvent(text, screen, track, BuildConfig.VERSION_NAME, System.currentTimeMillis())
        return fileWriter.appendLine(
            treeUri = treeUriStr.toUri(),
            relativePath = "gaps-phone-${syncPrefs.deviceId()}.jsonl",
            event = event,
            serializer = GapEvent.serializer(),
        )
    }

    companion object {
        /** 1000 UTF-16 units is at most ~3000 UTF-8 bytes: under PIPE_BUF with the other fields. */
        const val MAX_CHARS = 1000

        private val SHA256 = Regex("^[0-9a-f]{64}$")
        private val NEWLINES = Regex("[\\r\\n]+")

        fun normalize(text: String): String = text.replace(NEWLINES, " ").trim()

        fun buildEvent(text: String, screen: String, track: String?, app: String, ts: Long): GapEvent {
            val clean = normalize(text)
            require(clean.isNotEmpty()) { "gap text is empty" }
            require(clean.length <= MAX_CHARS) { "gap text over $MAX_CHARS chars: ${clean.length}" }
            return GapEvent(
                v = 1,
                ts = ts,
                text = clean,
                screen = screen,
                // Media ids are track shas today; anything else is dropped rather
                // than shipped as a malformed `track`.
                track = track?.takeIf { SHA256.matches(it) },
                app = app,
            )
        }
    }
}
