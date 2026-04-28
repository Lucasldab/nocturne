package io.nocturne.phone.data.prefs

import android.content.Context
import androidx.datastore.preferences.core.edit
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
     * Phase 6 (D-21, D-22): privacy-preserving 8-hex-char device identifier.
     * Generated exactly once on first access via SecureRandom (NOT IMEI / Android
     * ID / hostname / serial). Persisted to DataStore so reinstall + DataStore
     * restore preserves the same id; otherwise a fresh id is minted.
     *
     * The returned value is used to name JSONL files per docs/jsonl-spec.md §3:
     *   stats/phone-<deviceid>.jsonl
     *   likes-phone-<deviceid>.jsonl
     *   pins-phone-<deviceid>.jsonl
     */
    suspend fun deviceId(): String {
        val existing = ctx.syncDataStore.data.first()[DEVICE_ID]
        if (existing != null) return existing
        val bytes = ByteArray(4)
        SecureRandom().nextBytes(bytes)
        val newId = bytes.joinToString(separator = "") { "%02x".format(it.toInt() and 0xff) }
        ctx.syncDataStore.edit { it[DEVICE_ID] = newId }
        return newId
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
}
