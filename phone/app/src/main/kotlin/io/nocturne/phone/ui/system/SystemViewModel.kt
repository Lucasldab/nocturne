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

    private val _topArtists = MutableStateFlow<List<TopGroupRow>>(emptyList())
    val topArtists: StateFlow<List<TopGroupRow>> = _topArtists.asStateFlow()

    private val _topGenres = MutableStateFlow<List<TopGroupRow>>(emptyList())
    val topGenres: StateFlow<List<TopGroupRow>> = _topGenres.asStateFlow()

    private val _statsWindow = MutableStateFlow(StatsWindow.Week)
    val statsWindow: StateFlow<StatsWindow> = _statsWindow.asStateFlow()

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

    fun setStatsWindow(window: StatsWindow) {
        if (_statsWindow.value == window) return
        _statsWindow.value = window
        refreshStats()
    }

    fun refreshStats() {
        val window = _statsWindow.value
        viewModelScope.launch {
            val deviceId = container.syncPrefs.deviceId()
            val lines = withContext(Dispatchers.IO) { readStatsLines(deviceId) }
            val view = StatsAggregator.aggregate(
                lines.iterator(),
                System.currentTimeMillis(),
                windowMs = window.ms,
            )
            _stats.value = view
            // Resolve track titles for the top-played rows so the Stats screen
            // can render real titles. Bounded by topPlayed.take(10) upstream.
            val topPlayedTracks = withContext(Dispatchers.IO) {
                val dao = container.db.trackDao()
                buildMap {
                    view.topPlayed.forEach { row -> dao.byId(row.trackId)?.let { put(row.trackId, it) } }
                }
            }
            _topPlayedTracks.value = topPlayedTracks

            // Resolve every played track in batches, then group plays + listened
            // ms by artist[0] and genre[0]. Tracks not in the local DB (e.g.
            // legacy plays of tracks since removed from the catalog) are
            // skipped — we'd render the sha256 prefix and that's noise.
            val (artists, genres) = withContext(Dispatchers.IO) {
                val dao = container.db.trackDao()
                val ids = view.perTrackPlays.keys.toList()
                val resolved = mutableMapOf<String, TrackEntity>()
                ids.chunked(500).forEach { chunk ->
                    dao.byIds(chunk).forEach { resolved[it.id] = it }
                }
                val artistAgg = HashMap<String, Pair<Int, Long>>()  // plays, ms
                val genreAgg = HashMap<String, Pair<Int, Long>>()
                view.perTrackPlays.forEach { (trackId, plays) ->
                    val entity = resolved[trackId] ?: return@forEach
                    val ms = view.perTrackListenedMs[trackId] ?: 0L
                    val artistKey = entity.artist.firstOrNull()?.takeIf { it.isNotBlank() }
                    val genreKey = entity.genre.firstOrNull()?.takeIf { it.isNotBlank() }
                    if (artistKey != null) {
                        val prev = artistAgg[artistKey] ?: (0 to 0L)
                        artistAgg[artistKey] = (prev.first + plays) to (prev.second + ms)
                    }
                    if (genreKey != null) {
                        val prev = genreAgg[genreKey] ?: (0 to 0L)
                        genreAgg[genreKey] = (prev.first + plays) to (prev.second + ms)
                    }
                }
                val artistList = artistAgg.entries
                    .map { (k, v) -> TopGroupRow(label = k, plays = v.first, listenedMs = v.second) }
                    .sortedWith(
                        compareByDescending<TopGroupRow> { it.listenedMs }
                            .thenByDescending { it.plays },
                    )
                    .take(10)
                val genreList = genreAgg.entries
                    .map { (k, v) -> TopGroupRow(label = k, plays = v.first, listenedMs = v.second) }
                    .sortedWith(
                        compareByDescending<TopGroupRow> { it.listenedMs }
                            .thenByDescending { it.plays },
                    )
                    .take(10)
                artistList to genreList
            }
            _topArtists.value = artists
            _topGenres.value = genres
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

/**
 * Top-N row for artists / genres on the Stats screen. The label is the raw
 * artist[0] or genre[0] string from the matching TrackEntity.
 */
data class TopGroupRow(
    val label: String,
    val plays: Int,
    val listenedMs: Long,
)

/** Window selector for the Stats screen. */
enum class StatsWindow(val label: String, val ms: Long) {
    Week("7d", StatsAggregator.WEEK_MS),
    Month("30d", StatsAggregator.MONTH_MS),
    Year("1y", StatsAggregator.YEAR_MS),
}
