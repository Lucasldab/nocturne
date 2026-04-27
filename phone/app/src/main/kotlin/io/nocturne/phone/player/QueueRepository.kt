package io.nocturne.phone.player

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import androidx.media3.common.Player
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.flow.map
import kotlinx.serialization.SerializationException
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File

/**
 * Persists the player's queue across process death + reboot (PLAY-04).
 *
 * Data shape: a single JSON blob under one Preferences key. The blob carries
 * a `schema_version` so future plans can evolve the shape without crashing
 * older clients (forward-compat fallback to SavedQueue.EMPTY).
 *
 * Phase 5 schema_version = 1.
 *
 * Debounce: PlaybackService is responsible for debouncing calls to saveQueue
 * (500ms via Flow.debounce) so rapid Player.Listener events coalesce into a
 * single DataStore write.
 *
 * Pattern: mirrors phone/app/src/main/kotlin/io/nocturne/phone/data/prefs/SyncPrefs.kt
 * — same suspend-fun read/write API. Unlike SyncPrefs which uses a property
 * delegate, QueueRepository takes an optional `storeName` constructor param so
 * tests can inject isolated DataStore instances (unique names per test case).
 */

// Default production DataStore delegate (name = "nocturne_queue").
private val Context.queueDataStore by preferencesDataStore(name = "nocturne_queue")

@Serializable
data class SavedQueue(
    val schema_version: Int = SCHEMA_VERSION,
    val mediaIds: List<String> = emptyList(),
    val currentIndex: Int = 0,
    val currentPositionMs: Long = 0L,
    val shuffleMode: Boolean = false,
    val repeatMode: Int = Player.REPEAT_MODE_OFF,
) {
    companion object {
        const val SCHEMA_VERSION: Int = 1
        val EMPTY: SavedQueue = SavedQueue()
    }
}

class QueueRepository(
    private val ctx: Context,
    /**
     * Override the DataStore name. Production code always uses the default
     * ("nocturne_queue"). Tests pass a unique UUID-derived name so test cases
     * cannot share state even when Robolectric reuses the same Application.
     */
    storeName: String = "nocturne_queue",
) {

    private val dataStore: DataStore<Preferences> =
        if (storeName == "nocturne_queue") {
            // Use the property-delegate singleton for production — DataStore
            // requires exactly one instance per file per process.
            ctx.queueDataStore
        } else {
            // Test seam: build a file-backed DataStore with the given name.
            PreferenceDataStoreFactory.create(
                produceFile = { File(ctx.filesDir, "datastore/$storeName.preferences_pb") },
            )
        }

    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    /**
     * Read the persisted queue. Returns SavedQueue.EMPTY when:
     *   - no value has ever been written (first run / post-install)
     *   - the value is not parseable JSON (corruption — T-05-06-01)
     *   - the schema_version does not match SCHEMA_VERSION (forward-compat)
     *
     * Never throws. The on-boot resumption path MUST be crash-free.
     */
    suspend fun loadQueue(): SavedQueue {
        val raw = dataStore.data
            .map { prefs -> prefs[QUEUE_KEY] }
            .firstOrNull()
        return parseOrEmpty(raw)
    }

    /**
     * Observe the queue as a Flow. First emission is the current persisted
     * value (or EMPTY); subsequent emissions fire on writes from saveQueue.
     */
    fun observeQueue(): Flow<SavedQueue> =
        dataStore.data.map { prefs -> parseOrEmpty(prefs[QUEUE_KEY]) }

    /** Atomic write. Caller is responsible for debounce upstream. */
    suspend fun saveQueue(queue: SavedQueue) {
        val encoded = json.encodeToString(SavedQueue.serializer(), queue)
        dataStore.edit { it[QUEUE_KEY] = encoded }
    }

    /**
     * Test seam only: write a raw string under the queue key.
     * Used by QueueRepositoryTest to inject malformed or version-mismatched JSON.
     * NOT part of the public API; only called from tests.
     */
    internal suspend fun writeRawForTest(raw: String) {
        dataStore.edit { it[QUEUE_KEY] = raw }
    }

    private fun parseOrEmpty(raw: String?): SavedQueue {
        if (raw.isNullOrBlank()) return SavedQueue.EMPTY
        return try {
            val parsed = json.decodeFromString(SavedQueue.serializer(), raw)
            if (parsed.schema_version != SavedQueue.SCHEMA_VERSION) SavedQueue.EMPTY else parsed
        } catch (_: SerializationException) {
            SavedQueue.EMPTY
        } catch (_: IllegalArgumentException) {
            SavedQueue.EMPTY
        }
    }

    private companion object {
        val QUEUE_KEY: Preferences.Key<String> = stringPreferencesKey("queue_v1")
    }
}
