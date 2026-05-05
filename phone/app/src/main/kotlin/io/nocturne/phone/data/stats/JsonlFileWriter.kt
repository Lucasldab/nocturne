package io.nocturne.phone.data.stats

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.system.Os
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.serialization.KSerializer
import kotlinx.serialization.json.Json
import java.io.FileOutputStream
import java.io.IOException

/**
 * Phase 6 (D-01..D-04, D-26): single shared SAF append+fsync writer.
 *
 * StatsWriter / LikesWriter / PinsWriter are thin per-kind wrappers around
 * this class. Sharing one implementation eliminates byte-shape divergence.
 *
 * Spec discipline (docs/jsonl-spec.md §5, §6):
 *   - One JSON object per line, terminated by exactly one "\n" (LF). Hardcoded
 *     here as `"\n"`; never use the JVM platform line separator (Pitfall 4 —
 *     CRLF on Windows would silently break the spec).
 *   - Open SAF with mode "wa" (append). Fall back to "rw" + seek-to-end on
 *     IllegalArgumentException (some providers don't support "wa" — Pitfall 3).
 *   - Call android.system.Os.fsync(fileDescriptor) per event before returning.
 *     (ParcelFileDescriptor#sync() is not a public API on compileSdk 36; Os.fsync
 *     is the supported equivalent and what the JSONL spec §6 requires.)
 *   - On IOException: log and return false. Caller queues for retry; the spec
 *     accepts event loss under storage failure (§12).
 *
 * `Json` default config — no prettyPrint, no encodeDefaults, no naming
 * strategy. Per D-05.
 *
 * Updates SyncPrefs.lastStatsSyncAt on every successful fsync (D-26 / STATS-06).
 */
class JsonlFileWriter(
    private val context: Context,
    private val syncPrefs: SyncPrefs,
    private val nowMs: () -> Long = { System.currentTimeMillis() },
) {
    private val json = Json

    /**
     * Append a single JSONL line and fsync. Returns true on success.
     *
     * @param treeUri the user-granted SAF tree URI (must be SyncPrefs.metaTreeUri)
     * @param relativePath path under the tree, e.g. "stats/phone-a1b2c3d4.jsonl"
     */
    suspend fun <T : Any> appendLine(
        treeUri: Uri,
        relativePath: String,
        event: T,
        serializer: KSerializer<T>,
    ): Boolean {
        val encoded = json.encodeToString(serializer, event) + "\n"  // LF only (Pitfall 4)
        val bytes = encoded.toByteArray(Charsets.UTF_8)
        val fileUri = resolveOrCreateFile(treeUri, relativePath) ?: return false
        return try {
            writeAndFsync(fileUri, bytes)
            syncPrefs.setLastStatsSyncAt(nowMs())  // D-26
            true
        } catch (e: IOException) {
            Log.w("JsonlFileWriter", "append failed for $relativePath: ${e.message}")
            false
        } catch (e: SecurityException) {
            // Stale SAF grant — pre-0.4.40 builds only persisted READ. AppRoot's
            // upgrade check clears the URI so FirstRunScreen re-prompts; here we
            // just drop the event so the writer never escalates the SecurityException
            // into an app-killing crash mid-playback.
            Log.w("JsonlFileWriter", "SAF write denied for $relativePath: ${e.message}")
            false
        }
    }

    private fun writeAndFsync(fileUri: Uri, bytes: ByteArray) {
        // Primary path: SAF "wa" mode (Pitfall 3 / D-02).
        try {
            val pfdWa: ParcelFileDescriptor? = context.contentResolver.openFileDescriptor(fileUri, "wa")
            if (pfdWa != null) {
                try {
                    FileOutputStream(pfdWa.fileDescriptor).use { fos -> fos.write(bytes) }
                    Os.fsync(pfdWa.fileDescriptor)  // fsync(2) — D-03
                } finally {
                    pfdWa.close()
                }
                return
            }
        } catch (_: IllegalArgumentException) {
            // Fallback: provider doesn't support "wa". Open "rw", seek to end, write, fsync.
        }
        val pfdRw: ParcelFileDescriptor =
            context.contentResolver.openFileDescriptor(fileUri, "rw") ?: return
        try {
            FileOutputStream(pfdRw.fileDescriptor).use { fos ->
                val ch = fos.channel
                ch.position(ch.size())
                fos.write(bytes)
            }
            Os.fsync(pfdRw.fileDescriptor)  // fsync(2) — D-03
        } finally {
            pfdRw.close()
        }
    }

    /**
     * Resolve the file at `relativePath` under the SAF `treeUri`, creating any
     * missing directories along the way. Returns null if any segment cannot be
     * created (caller logs, drops the event for retry).
     */
    private fun resolveOrCreateFile(treeUri: Uri, relativePath: String): Uri? {
        var node: DocumentFile = DocumentFile.fromTreeUri(context, treeUri) ?: return null
        val parts = relativePath.split("/")
        for (segment in parts.dropLast(1)) {
            node = node.findFile(segment)
                ?: node.createDirectory(segment)
                ?: return null
        }
        val leaf = parts.last()
        val existing = node.findFile(leaf)
        if (existing != null) return existing.uri
        return node.createFile("application/octet-stream", leaf)?.uri
    }
}
