package io.nocturne.phone.data.prefs

import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.syncDataStore by preferencesDataStore(name = "nocturne_sync")
private val META_TREE_URI = stringPreferencesKey("meta_tree_uri")
private val LAST_IMPORT_AT = stringPreferencesKey("last_import_at_iso")

/**
 * Single-purpose DataStore wrapper. Two keys:
 *  - `meta_tree_uri`: the SAF tree URI of the metadata folder (Syncthing-
 *    delivered `catalog.json` + `manifest.json` live here). Persisted across
 *    launches so the user only picks the folder once.
 *  - `last_import_at_iso`: ISO-8601 timestamp of the last successful import.
 *    Plan 04-04 surfaces this on the import screen.
 */
class SyncPrefs(private val ctx: Context) {
    val metaTreeUri: Flow<String?> =
        ctx.syncDataStore.data.map { it[META_TREE_URI] }
    val lastImportAt: Flow<String?> =
        ctx.syncDataStore.data.map { it[LAST_IMPORT_AT] }

    suspend fun setMetaTreeUri(uri: String) {
        ctx.syncDataStore.edit { it[META_TREE_URI] = uri }
    }

    suspend fun setLastImportAt(iso: String) {
        ctx.syncDataStore.edit { it[LAST_IMPORT_AT] = iso }
    }

    suspend fun clearMetaTreeUri() {
        ctx.syncDataStore.edit { it.remove(META_TREE_URI) }
    }
}
