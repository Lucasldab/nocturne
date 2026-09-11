package io.nocturne.phone.ui.home

import androidx.core.net.toUri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import io.nocturne.phone.data.AppContainer
import io.nocturne.phone.data.catalog.DiscoveryJson
import io.nocturne.phone.data.catalog.DiscoveryPick
import io.nocturne.phone.data.catalog.Recommendation
import io.nocturne.phone.data.catalog.RecommendationsJson
import io.nocturne.phone.data.catalog.SafCatalogSource
import io.nocturne.phone.data.db.entity.DownloadEntity
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.serialization.json.Json
import java.util.UUID

data class HomeState(
    val loading: Boolean = true,
    val picks: List<DiscoveryPick> = emptyList(),
    val recommendations: List<Recommendation> = emptyList(),
    /** Queries already requested this session — drives the row's label. */
    val requested: Set<String> = emptySet(),
    val note: String? = null,
)

/**
 * Home / For You.
 *
 * Reads two files the desktop publishes into the meta tree and Syncthing
 * delivers: discovery.json (tracks you own but have not heard) and
 * recommendations.json (artists you do NOT own). Both are plain files — the app
 * has no INTERNET permission (CROSS-01), so every query it might need is
 * pre-built on the desktop.
 *
 * Tapping a row writes a DownloadEntity; DownloadsWriter drains it to
 * downloads-phone-*.jsonl, the daemon's `download` command execs flacget, and
 * status comes back through downloads-desktop.jsonl.
 */
class HomeViewModel(private val container: AppContainer) : ViewModel() {

    private val json = Json { ignoreUnknownKeys = true; isLenient = true }
    private val _state = MutableStateFlow(HomeState())
    val state: StateFlow<HomeState> = _state.asStateFlow()

    init {
        refresh()
    }

    fun refresh() {
        viewModelScope.launch(Dispatchers.IO) {
            val treeUri = container.syncPrefs.metaTreeUri.first()
            if (treeUri == null) {
                _state.value = HomeState(loading = false, note = "no metadata folder picked")
                return@launch
            }
            val src = SafCatalogSource(container.appContext, treeUri.toUri())
            val picks = runCatching {
                src.openDiscovery()?.use {
                    json.decodeFromString<DiscoveryJson>(it.readBytes().decodeToString()).picks
                }
            }.getOrNull().orEmpty()
            val recs = runCatching {
                src.openRecommendations()?.use {
                    json.decodeFromString<RecommendationsJson>(
                        it.readBytes().decodeToString(),
                    ).recommendations
                }
            }.getOrNull().orEmpty()

            val note = when {
                picks.isEmpty() && recs.isEmpty() ->
                    "nothing published yet — the desktop writes these on its weekly run"
                else -> null
            }
            _state.value = HomeState(
                loading = false,
                picks = picks,
                recommendations = recs,
                requested = _state.value.requested,
                note = note,
            )
        }
    }

    /** Request a download. Safe to tap twice — the row is keyed by query. */
    fun fetch(query: String) {
        if (query.isBlank() || query in _state.value.requested) return
        _state.value = _state.value.copy(requested = _state.value.requested + query)
        viewModelScope.launch(Dispatchers.IO) {
            container.db.downloadDao().upsert(
                DownloadEntity(
                    id = UUID.randomUUID().toString(),
                    query = query,
                    requestedAt = System.currentTimeMillis(),
                ),
            )
            // Push it out immediately rather than waiting for the next drain.
            runCatching { container.downloadsWriter.drain() }
        }
    }
}

class HomeVMFactory(private val container: AppContainer) : ViewModelProvider.Factory {
    @Suppress("UNCHECKED_CAST")
    override fun <T : ViewModel> create(modelClass: Class<T>): T =
        HomeViewModel(container) as T
}
