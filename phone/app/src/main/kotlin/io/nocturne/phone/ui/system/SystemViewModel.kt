package io.nocturne.phone.ui.system

import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.catalog.ManifestJson
import io.nocturne.phone.data.db.entity.TrackEntity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import java.io.BufferedReader
import java.io.InputStreamReader

/**
 * Quick task 260428-7zc — single ViewModel for the four System sub-screens.
 *
 * Exposes three StateFlows:
 *  - [rotation]: bucket roll-up + cap/used totals from manifest.json
 *  - [stats]: 7d window aggregate over phone-<deviceid>.jsonl
 *  - [topPlayedTracks]: TrackEntity lookup map for the top-played list (so
 *    the Stats screen can render real titles instead of sha256 prefixes)
 *
 * No direct network calls — all I/O goes through the user-granted SAF tree
 * URI and Room. Honors the CROSS-03 / audit-network gate.
 *
 * `refreshRotation()` and `refreshStats()` are idempotent; the screens call
 * them in a `LaunchedEffect(Unit)` so re-entry rebuilds the view.
 */
class SystemViewModel(private val container: AppContainer) : ViewModel() {

    private val json = Json { ignoreUnknownKeys = true }

    private val _rotation = MutableStateFlow(RotationView.empty)
    val rotation: StateFlow<RotationView> = _rotation.asStateFlow()

    private val _stats = MutableStateFlow(StatsView.empty())
    val stats: StateFlow<StatsView> = _stats.asStateFlow()

    private val _topPlayedTracks = MutableStateFlow<Map<String, TrackEntity>>(emptyMap())
    val topPlayedTracks: StateFlow<Map<String, TrackEntity>> = _topPlayedTracks.asStateFlow()

    fun refreshRotation() {
        viewModelScope.launch {
            val manifest = withContext(Dispatchers.IO) { loadManifest() }
            val tracksById: Map<String, TrackEntity> = withContext(Dispatchers.IO) {
                if (manifest == null) {
                    emptyMap()
                } else {
                    val ids = manifest.resident.map { it.id }.toSet()
                    val dao = container.db.trackDao()
                    buildMap {
                        ids.forEach { id -> dao.byId(id)?.let { put(id, it) } }
                    }
                }
            }
            _rotation.value = RotationAggregator.aggregate(manifest, tracksById)
        }
    }

    fun refreshStats() {
        viewModelScope.launch {
            val deviceId = container.syncPrefs.deviceId()
            val lines = withContext(Dispatchers.IO) { readStatsLines(deviceId) }
            val view = StatsAggregator.aggregate(lines.iterator(), System.currentTimeMillis())
            _stats.value = view
            // Resolve track titles for the top-played rows so the Stats screen
            // can render real titles. Bounded by topPlayed.take(10) upstream.
            val tracks = withContext(Dispatchers.IO) {
                val dao = container.db.trackDao()
                buildMap {
                    view.topPlayed.forEach { row -> dao.byId(row.trackId)?.let { put(row.trackId, it) } }
                }
            }
            _topPlayedTracks.value = tracks
        }
    }

    private suspend fun loadManifest(): ManifestJson? {
        val uriStr = container.syncPrefs.metaTreeUri.first() ?: return null
        val tree = DocumentFile.fromTreeUri(container.appContext, Uri.parse(uriStr)) ?: return null
        val f = tree.findFile("manifest.json") ?: return null
        return container.appContext.contentResolver.openInputStream(f.uri)?.use { ins ->
            BufferedReader(InputStreamReader(ins, Charsets.UTF_8)).use { r ->
                val text = r.readText()
                runCatching { json.decodeFromString(ManifestJson.serializer(), text) }
                    .getOrNull()
            }
        }
    }

    private suspend fun readStatsLines(deviceId: String): List<String> {
        val uriStr = container.syncPrefs.metaTreeUri.first() ?: return emptyList()
        val tree = DocumentFile.fromTreeUri(container.appContext, Uri.parse(uriStr)) ?: return emptyList()
        val statsDir = tree.findFile("stats") ?: return emptyList()
        val f = statsDir.findFile("phone-$deviceId.jsonl") ?: return emptyList()
        return container.appContext.contentResolver.openInputStream(f.uri)?.use { ins ->
            BufferedReader(InputStreamReader(ins, Charsets.UTF_8)).use { it.readLines() }
        } ?: emptyList()
    }

    class Factory(private val container: AppContainer) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T =
            SystemViewModel(container) as T
    }
}
