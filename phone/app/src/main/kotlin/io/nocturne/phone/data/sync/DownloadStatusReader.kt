package io.nocturne.phone.data.sync

import android.content.Context
import androidx.core.net.toUri
import androidx.documentfile.provider.DocumentFile
import io.nocturne.phone.data.db.dao.DownloadDao
import io.nocturne.phone.data.prefs.SyncPrefs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.io.BufferedReader
import java.io.InputStreamReader

/**
 * Reads `<metaTree>/downloads-desktop.jsonl` and applies each status line
 * to the local downloads Room table via [DownloadDao.updateStatus].
 *
 * Stateless (and simple): re-parses the file from byte zero each tick.
 * The status JSONL is sparse — one or two lines per request, bounded by
 * the number of requests the user ever submitted — so re-reading is cheap
 * even after months of usage. LWW semantics on `statusUpdatedAt` mean
 * re-applying an old line is a no-op.
 *
 * Call sites:
 *   - SyncScreen periodic poll (5s while the section is visible)
 *   - DownloadsViewModel.submit() — re-poll once shortly after submit so
 *     the user sees the "running" transition without waiting for the next
 *     tick.
 */
class DownloadStatusReader(
    private val context: Context,
    private val downloadDao: DownloadDao,
    private val syncPrefs: SyncPrefs,
) {
    private val json = Json { ignoreUnknownKeys = true }

    /** Tail the status file once. Returns the number of status lines applied. */
    suspend fun poll(): Int = withContext(Dispatchers.IO) {
        val treeStr = syncPrefs.metaTreeUri.first() ?: return@withContext 0
        val tree = DocumentFile.fromTreeUri(context, treeStr.toUri())
            ?: return@withContext 0
        val file = tree.findFile("downloads-desktop.jsonl") ?: return@withContext 0
        var applied = 0
        runCatching {
            context.contentResolver.openInputStream(file.uri)?.use { ins ->
                BufferedReader(InputStreamReader(ins, Charsets.UTF_8)).useLines { lines ->
                    for (raw in lines) {
                        if (raw.isBlank()) continue
                        val parsed = runCatching {
                            json.parseToJsonElement(raw).jsonObject
                        }.getOrNull() ?: continue
                        val id = parsed["id"]?.jsonPrimitive?.contentOrNullSafe() ?: continue
                        val state = parsed["state"]?.jsonPrimitive?.contentOrNullSafe() ?: continue
                        val ts = parsed["ts"]?.jsonPrimitive?.contentOrNullSafe()?.toLongOrNull() ?: 0L
                        val msg = parsed["msg"]?.jsonPrimitive?.contentOrNullSafe()
                        downloadDao.updateStatus(id = id, state = state, message = msg, ts = ts)
                        applied++
                    }
                }
            }
        }.onFailure {
            android.util.Log.w("DownloadStatusReader", "poll failed: ${it.message}")
        }
        applied
    }

    private fun kotlinx.serialization.json.JsonPrimitive.contentOrNullSafe(): String? =
        if (isString) content else content.takeUnless { it == "null" }
}
