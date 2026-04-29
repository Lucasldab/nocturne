package io.nocturne.phone.data.prefs

import android.content.Context
import androidx.core.net.toUri
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import java.security.SecureRandom

private val Context.syncDataStore by preferencesDataStore(name = "nocturne_sync")
private val META_TREE_URI = stringPreferencesKey("meta_tree_uri")
private val MUSIC_TREE_URI = stringPreferencesKey("music_tree_uri")
private val LAST_IMPORT_AT = stringPreferencesKey("last_import_at_iso")
private val DEVICE_ID = stringPreferencesKey("device_id")
private val LAST_STATS_SYNC_AT = longPreferencesKey("last_stats_sync_at_ms")
private val STORAGE_BUDGET_GB = intPreferencesKey("storage_budget_gb")
private val NOTIF_PROMPT_SHOWN = booleanPreferencesKey("notif_prompt_shown")

/**
 * Single-purpose DataStore wrapper. Keys:
 *  - `meta_tree_uri`: the SAF tree URI of the metadata folder (Syncthing-
 *    delivered `catalog.json` + `manifest.json` live here). Persisted across
 *    launches so the user only picks the folder once.
 *  - `last_import_at_iso`: ISO-8601 timestamp of the last successful import.
 *    Plan 04-04 surfaces this on the import screen.
 *  - `device_id` (Phase 6): 8-char lowercase-hex token from 4 SecureRandom
 *    bytes — used to name JSONL files per docs/jsonl-spec.md §3.
 *  - `last_stats_sync_at_ms` (Phase 6): epoch-ms timestamp of the last
 *    successful JSONL append; surfaces in SettingsScreen (STATS-06).
 */
class SyncPrefs(private val ctx: Context) {
    val metaTreeUri: Flow<String?> =
        ctx.syncDataStore.data.map { it[META_TREE_URI] }
    val musicTreeUri: Flow<String?> =
        ctx.syncDataStore.data.map { it[MUSIC_TREE_URI] }
    val lastImportAt: Flow<String?> =
        ctx.syncDataStore.data.map { it[LAST_IMPORT_AT] }

    suspend fun setMetaTreeUri(uri: String) {
        ctx.syncDataStore.edit { it[META_TREE_URI] = uri }
    }

    suspend fun setMusicTreeUri(uri: String) {
        ctx.syncDataStore.edit { it[MUSIC_TREE_URI] = uri }
    }

    suspend fun setLastImportAt(iso: String) {
        ctx.syncDataStore.edit { it[LAST_IMPORT_AT] = iso }
    }

    suspend fun clearMetaTreeUri() {
        ctx.syncDataStore.edit { it.remove(META_TREE_URI) }
    }

    suspend fun clearMusicTreeUri() {
        ctx.syncDataStore.edit { it.remove(MUSIC_TREE_URI) }
    }

    /**
     * Phase 6 (D-21, D-22) + 260428 reinstall persistence: privacy-preserving
     * 8-hex-char device identifier. NOT IMEI / Android ID / hostname / serial.
     *
     * Lookup priority (first hit wins, all subsequent levels propagate up):
     *   1. DataStore `device_id` key — fastest, no SAF I/O.
     *   2. `<metaTreeUri>/.nocturne-deviceid` (SAF) — survives reinstall as
     *      long as the user re-picks the same meta tree. Avoids the
     *      "every Obtainium upgrade gets a fresh device-id" problem caused by
     *      `android:allowBackup="false"` wiping DataStore on app data clear.
     *   3. Mint a fresh SecureRandom 4-byte hex.
     *
     * On levels 2/3, also write back to DataStore so subsequent calls hit
     * level 1. On level 3, also try to write to the SAF tree so the next
     * reinstall finds it at level 2.
     *
     * The returned value is used to name JSONL files per docs/jsonl-spec.md §3:
     *   stats/phone-<deviceid>.jsonl
     *   likes-phone-<deviceid>.jsonl
     *   pins-phone-<deviceid>.jsonl
     */
    suspend fun deviceId(): String {
        // Level 1 — DataStore.
        val cached = ctx.syncDataStore.data.first()[DEVICE_ID]
        if (cached != null) return cached

        // Level 2 — SAF tree's `.nocturne-deviceid` file.
        val safId = readDeviceIdFromSaf()
        if (safId != null) {
            ctx.syncDataStore.edit { it[DEVICE_ID] = safId }
            return safId
        }

        // Level 3 — mint fresh + persist to both places.
        val bytes = ByteArray(4)
        SecureRandom().nextBytes(bytes)
        val newId = bytes.joinToString(separator = "") { "%02x".format(it.toInt() and 0xff) }
        ctx.syncDataStore.edit { it[DEVICE_ID] = newId }
        runCatching { writeDeviceIdToSaf(newId) }
        return newId
    }

    private suspend fun readDeviceIdFromSaf(): String? {
        val treeStr = metaTreeUri.first() ?: return null
        val tree = androidx.documentfile.provider.DocumentFile.fromTreeUri(
            ctx, treeStr.toUri(),
        ) ?: return null
        val file = tree.findFile(".nocturne-deviceid") ?: return null
        return runCatching {
            ctx.contentResolver.openInputStream(file.uri)?.use { it.readBytes() }
                ?.toString(Charsets.UTF_8)
                ?.trim()
                ?.takeIf { it.matches(Regex("^[0-9a-f]{8}$")) }
        }.getOrNull()
    }

    private suspend fun writeDeviceIdToSaf(id: String) {
        val treeStr = metaTreeUri.first() ?: return
        val tree = androidx.documentfile.provider.DocumentFile.fromTreeUri(
            ctx, treeStr.toUri(),
        ) ?: return
        val existing = tree.findFile(".nocturne-deviceid")
        val target = existing ?: tree.createFile("application/octet-stream", ".nocturne-deviceid") ?: return
        ctx.contentResolver.openOutputStream(target.uri, "wt")?.use {
            it.write(id.toByteArray(Charsets.UTF_8))
        }
    }

    /**
     * Phase 6 (STATS-06 / D-26): epoch-ms timestamp of the last successful JSONL
     * append. Updated by JsonlFileWriter on every fsync. Read by SettingsScreen
     * to render "Last event logged: <relative time>".
     */
    val lastStatsSyncAt: Flow<Long?> =
        ctx.syncDataStore.data.map { it[LAST_STATS_SYNC_AT] }

    suspend fun setLastStatsSyncAt(epochMs: Long) {
        ctx.syncDataStore.edit { it[LAST_STATS_SYNC_AT] = epochMs }
    }

    /**
     * Storage screen: user-configurable resident-set byte budget in whole
     * gigabytes. Default 12 GB matches the project's "default ~12GB"
     * constraint. Range clamped to 4..32 GB on
     * write — the slider in StorageScreen renders the same domain so
     * out-of-range values can only arrive via direct DataStore tampering.
     *
     * NOTE: this preference is the phone-side display target only. The
     * authoritative cap that drives rotation lives on the desktop daemon's
     * config.toml; this value is currently advisory until a future plan
     * pushes it back via the meta folder.
     */
    val storageBudgetGb: Flow<Int> =
        ctx.syncDataStore.data.map { it[STORAGE_BUDGET_GB] ?: 12 }

    suspend fun setStorageBudgetGb(gb: Int) {
        val clamped = gb.coerceIn(4, 32)
        ctx.syncDataStore.edit { it[STORAGE_BUDGET_GB] = clamped }
    }

    /**
     * POST_NOTIFICATIONS gate: true once the
     * AppRoot-hosted FirstPlayNotifGate has shown its rationale dialog
     * for the first time. The gate persists this on EVERY terminal
     * state (Allow tapped + system dialog resolved, "Not now" tapped,
     * outside-tap dismiss) so the rationale never repeats. Pre-Android-13
     * never reaches the gate; this flag stays false on those builds and
     * never affects behavior.
     */
    val notifPromptShown: Flow<Boolean> =
        ctx.syncDataStore.data.map { it[NOTIF_PROMPT_SHOWN] ?: false }

    suspend fun setNotifPromptShown(shown: Boolean) {
        ctx.syncDataStore.edit { it[NOTIF_PROMPT_SHOWN] = shown }
    }
}
